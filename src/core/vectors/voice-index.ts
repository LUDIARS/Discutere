/**
 * 外部の声の埋め込みインデックス書き込み (spec/feature/voice-rag-hybrid.md)。
 *
 * offline バッチ (`scripts/build-voice-embeddings.ts`) と、議論準備中の収集直後の
 * 部分ベクトル化 (`flow/user-voices/collect.ts`) の両方がこの関数で書き込む。
 * 1 バッチ 1 トランザクションで upsert するので、途中で止まっても再実行で続きから埋まる。
 */

import type { createCore } from "../index.js";
import type { EmbeddingClient } from "./embedder.js";

type Core = ReturnType<typeof createCore>;

/** 埋め込み対象テキストの上限 (長文はモデルの実効長に合わせて先頭を使う)。 */
export const EMBED_TEXT_CAP = 2000;

export interface VoiceRow {
  id: string;
  raw_content: string | null;
}

/** 行を埋め込んで embeddings (nodeType='utterance') に upsert する。@returns 書き込んだ件数。 */
export async function embedVoiceRows(
  core: Core,
  embedder: EmbeddingClient,
  rows: readonly VoiceRow[],
  opts: { workspaceId: string; batchSize: number; onProgress?: (done: number, total: number) => void }
): Promise<number> {
  const targets = rows.filter((r) => (r.raw_content ?? "").trim().length > 0);
  const batchSize = Math.max(1, opts.batchSize);
  let done = 0;
  for (let i = 0; i < targets.length; i += batchSize) {
    const batch = targets.slice(i, i + batchSize);
    const vectors = await embedder.embed(batch.map((r) => (r.raw_content ?? "").slice(0, EMBED_TEXT_CAP)));
    const insert = core.client.raw.transaction((items: Array<{ id: string; vec: number[] }>) => {
      for (const item of items) {
        core.vectors.registerEmbedding({
          workspaceId: opts.workspaceId,
          nodeType: "utterance",
          nodeId: item.id,
          vector: item.vec,
        });
      }
    });
    insert(batch.map((r, j) => ({ id: r.id, vec: vectors[j] })));
    done += batch.length;
    opts.onProgress?.(done, targets.length);
  }
  return done;
}

/**
 * 指定 id の外部の声のうち、まだベクトルが無いものだけを埋め込む (収集直後の部分ベクトル化)。
 * @returns 書き込んだ件数。
 */
export async function embedVoicesByIds(
  core: Core,
  embedder: EmbeddingClient,
  ids: readonly string[],
  opts: { workspaceId: string; batchSize: number }
): Promise<number> {
  if (ids.length === 0) return 0;
  const select = core.client.raw.prepare(
    `SELECT u.id, u.raw_content
       FROM utterances u
       LEFT JOIN embeddings e
         ON e.workspace_id = u.workspace_id AND e.node_type = 'utterance' AND e.node_id = u.id
      WHERE u.workspace_id = ? AND u.id = ? AND e.node_id IS NULL`
  );
  const rows: VoiceRow[] = [];
  for (const id of new Set(ids)) {
    const row = select.get(opts.workspaceId, id) as VoiceRow | undefined;
    if (row) rows.push(row);
  }
  return embedVoiceRows(core, embedder, rows, opts);
}
