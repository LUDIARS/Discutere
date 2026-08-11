/**
 * 外部の声の埋め込みインデックス構築 (offline バッチ)。
 *
 *   npm run build:voice-embeddings              # 未埋め込み分を全部
 *   npm run build:voice-embeddings -- --limit 5000   # 件数を絞って様子見
 *
 * active KG の外部発話 (speaker_id='ext:%') のうち embeddings テーブルに
 * ベクトルが無いものを config.embedding のモデルで埋め込み、
 * registerEmbedding (nodeType='utterance') で upsert する。増分実行可能
 * (再実行は未埋め込み分だけを処理する)。
 *
 * ハイブリッド RAG (voice-search.ts) はこのインデックスを前提とする。
 * 未構築でもキーワード検索で動くが、ベクトル rerank は効かない。
 *
 * モデルを変えた場合は既存ベクトルと空間が混ざるため、--rebuild で全削除→再構築する。
 */

import { getConfig } from "../src/config.js";
import { createCore } from "../src/core/index.js";
import { resolveActiveKgPath } from "../src/core/kg-registry.js";
import { createOpenAiCompatEmbedder } from "../src/core/vectors/embedder.js";

function parseArgs(): { limit: number; rebuild: boolean } {
  const args = process.argv.slice(2);
  const limitIdx = args.indexOf("--limit");
  const limit = limitIdx >= 0 ? Number(args[limitIdx + 1]) : Infinity;
  if (!Number.isInteger(limit) || limit <= 0) {
    console.error("--limit には正の整数を指定する");
    process.exit(1);
  }
  return { limit, rebuild: args.includes("--rebuild") };
}

/** 埋め込み対象テキスト: 長文はモデルの実効長に合わせて先頭を使う (bge-m3 は 8k token だが安全側)。 */
const EMBED_TEXT_CAP = 2000;

/** @implements SPEC-VOICE-RAG-HYBRID-OPS */
async function main(): Promise<void> {
  const { limit, rebuild } = parseArgs();
  const config = getConfig();
  // 専用スクリプトなので前提は入口で検証する (fail-fast)。enabled=false でも
  // インデックス構築自体は許可する (構築後に enabled を立てる運用があるため)。
  const embedder = createOpenAiCompatEmbedder(config.embedding);
  try {
    await embedder.embed(["疎通確認"]);
  } catch (e) {
    console.error(
      `埋め込みエンドポイントに疎通できない (model=${config.embedding.model}): ${(e as Error).message}`
    );
    console.error("Ollama なら `ollama pull bge-m3` 済みか、サーバ起動を確認する。");
    process.exit(1);
  }

  const core = createCore(resolveActiveKgPath(config));
  try {
    const workspaceId = config.workspace;
    if (rebuild) {
      const del = core.client.raw
        .prepare("DELETE FROM embeddings WHERE workspace_id = ? AND node_type = 'utterance'")
        .run(workspaceId);
      console.log(`--rebuild: 既存 utterance ベクトル ${del.changes} 件を削除`);
    }

    const pending = core.client.raw
      .prepare(
        `SELECT u.id, u.raw_content
           FROM utterances u
           LEFT JOIN embeddings e
             ON e.workspace_id = u.workspace_id AND e.node_type = 'utterance' AND e.node_id = u.id
          WHERE u.workspace_id = ? AND u.speaker_id LIKE 'ext:%' AND e.node_id IS NULL
          ORDER BY u.posted_at DESC`
      )
      .all(workspaceId) as Array<{ id: string; raw_content: string | null }>;

    const targets = pending
      .filter((r) => (r.raw_content ?? "").trim().length > 0)
      .slice(0, limit === Infinity ? pending.length : limit);
    console.log(`未埋め込みの外部の声: ${pending.length} 件 (今回処理: ${targets.length} 件)`);

    const batchSize = config.embedding.batchSize;
    let done = 0;
    for (let i = 0; i < targets.length; i += batchSize) {
      const batch = targets.slice(i, i + batchSize);
      const vectors = await embedder.embed(
        batch.map((r) => (r.raw_content ?? "").slice(0, EMBED_TEXT_CAP))
      );
      // 1 バッチ 1 トランザクションで書く (途中中断しても再実行で続きから)。
      const insert = core.client.raw.transaction((rows: Array<{ id: string; vec: number[] }>) => {
        for (const row of rows) {
          core.vectors.registerEmbedding({
            workspaceId,
            nodeType: "utterance",
            nodeId: row.id,
            vector: row.vec,
          });
        }
      });
      insert(batch.map((r, j) => ({ id: r.id, vec: vectors[j] })));
      done += batch.length;
      if (done % (batchSize * 10) === 0 || done === targets.length) {
        console.log(`  ${done}/${targets.length} 件 完了`);
      }
    }
    console.log(`埋め込み構築完了: ${done} 件 (model=${config.embedding.model})`);
  } finally {
    core.close?.();
  }
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
