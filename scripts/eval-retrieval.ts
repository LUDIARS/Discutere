/**
 * 外部の声 RAG の検索精度評価ハーネス。
 *
 *   npm run eval:retrieval                       # data/eval/retrieval-golden.json を評価
 *   npm run eval:retrieval -- --golden path.json # ゴールデンセット指定
 *
 * ゴールデンセット (JSON) の各ケースについて、
 *   - keyword: キーワード検索のみ (クエリ埋め込み未使用)
 *   - hybrid : クエリ埋め込みを温めてからハイブリッド検索
 * の 2 モードで検索し、次を比較する:
 *   - hits          : 返却された声のうち relevantMarkers のいずれかを含む件数
 *   - markerCoverage: relevantMarkers のうち少なくとも 1 件の声に現れた割合
 *   - recallAtK     : relevantIds 指定時のみ (正解 id の回収率)
 *
 * 「精度が上がった」を主観でなく数字で判定するための基盤。改善前後で
 * この出力を比較する。ゴールデンセットは実 KG に合わせて育てる
 * (雛形: data/eval/retrieval-golden.example.json)。
 */

import fs from "node:fs";
import path from "node:path";

import { getConfig } from "../src/config.js";
import { createCore } from "../src/core/index.js";
import { resolveActiveKgPath } from "../src/core/kg-registry.js";
import { createOpenAiCompatEmbedder } from "../src/core/vectors/embedder.js";
import {
  normalizeQueryText,
  warmQueryEmbedding,
} from "../src/core/vectors/query-embed-cache.js";
import { searchExternalVoiceRows } from "../src/discatier-engine-adapter/voice-search.js";

interface GoldenCase {
  theme: string;
  k?: number;
  /** 「関連している」とみなす本文マーカー (いずれかを含めば関連)。 */
  relevantMarkers?: string[];
  /** 明示ラベル付けした正解 utterance id (あれば recall@k を出す)。 */
  relevantIds?: string[];
  note?: string;
}

interface GoldenFile {
  cases: GoldenCase[];
}

function isStringArray(value: unknown): value is string[] {
  return Array.isArray(value) && value.every((item) => typeof item === "string");
}

function parseGoldenFile(text: string): GoldenFile {
  const value: unknown = JSON.parse(text);
  if (!value || typeof value !== "object" || !Array.isArray((value as { cases?: unknown }).cases)) {
    throw new Error("golden file の cases は配列で指定する");
  }
  const cases = (value as { cases: unknown[] }).cases;
  for (const [index, item] of cases.entries()) {
    if (!item || typeof item !== "object") throw new Error(`cases[${index}] は object で指定する`);
    const c = item as Record<string, unknown>;
    if (typeof c.theme !== "string" || c.theme.trim().length === 0) {
      throw new Error(`cases[${index}].theme は空でない文字列で指定する`);
    }
    if (c.k !== undefined && (typeof c.k !== "number" || !Number.isInteger(c.k) || c.k <= 0)) {
      throw new Error(`cases[${index}].k は正の整数で指定する`);
    }
    if (c.relevantMarkers !== undefined && !isStringArray(c.relevantMarkers)) {
      throw new Error(`cases[${index}].relevantMarkers は文字列配列で指定する`);
    }
    if (c.relevantIds !== undefined && !isStringArray(c.relevantIds)) {
      throw new Error(`cases[${index}].relevantIds は文字列配列で指定する`);
    }
    if (c.note !== undefined && typeof c.note !== "string") {
      throw new Error(`cases[${index}].note は文字列で指定する`);
    }
  }
  return { cases: cases as GoldenCase[] };
}

interface ModeResult {
  returned: number;
  hits: number;
  markerCoverage: number;
  recallAtK: number | null;
}

function evaluateResult(
  rows: ReadonlyArray<{ id: string; rawContent: string }>,
  c: GoldenCase
): ModeResult {
  const markers = (c.relevantMarkers ?? []).map((m) => m.toLowerCase());
  const contents = rows.map((r) => r.rawContent.toLowerCase());
  const hits = markers.length
    ? contents.filter((t) => markers.some((m) => t.includes(m))).length
    : 0;
  const covered = markers.length
    ? markers.filter((m) => contents.some((t) => t.includes(m))).length / markers.length
    : 0;
  let recallAtK: number | null = null;
  if (c.relevantIds && c.relevantIds.length > 0) {
    const got = new Set(rows.map((r) => r.id));
    recallAtK = c.relevantIds.filter((id) => got.has(id)).length / c.relevantIds.length;
  }
  return { returned: rows.length, hits, markerCoverage: covered, recallAtK };
}

function fmt(r: ModeResult): string {
  const recall = r.recallAtK === null ? "-" : r.recallAtK.toFixed(2);
  return `returned=${r.returned} hits=${r.hits} markerCov=${r.markerCoverage.toFixed(2)} recall@k=${recall}`;
}

/** @implements SPEC-VOICE-RAG-HYBRID-EVAL */
async function main(): Promise<void> {
  const args = process.argv.slice(2);
  const goldenIdx = args.indexOf("--golden");
  const goldenArg = goldenIdx >= 0 ? args[goldenIdx + 1] : "data/eval/retrieval-golden.json";
  if (!goldenArg || goldenArg.startsWith("--")) {
    console.error("--golden には JSON ファイルのパスを指定する。");
    process.exit(1);
  }
  const goldenPath = path.resolve(goldenArg);
  if (!fs.existsSync(goldenPath)) {
    console.error(`ゴールデンセットが見つからない: ${goldenPath}`);
    console.error("雛形 data/eval/retrieval-golden.example.json をコピーして実 KG に合わせて作る。");
    process.exit(1);
  }
  const golden = parseGoldenFile(fs.readFileSync(goldenPath, "utf-8"));
  if (golden.cases.length === 0) {
    console.error("cases が空。");
    process.exit(1);
  }

  const config = getConfig();
  if (!config.embedding.enabled) {
    console.warn(
      "[warn] config.embedding.enabled=false — hybrid モードは keyword と同じ結果になる。" +
        " DISCUTERE_EMBEDDING_ENABLED=1 で有効化する。"
    );
  }
  const embedder = createOpenAiCompatEmbedder(config.embedding);
  const core = createCore(resolveActiveKgPath(config));
  const warn = (m: string): void => console.warn(`[eval] ${m}`);
  try {
    let sumKw = { hits: 0, cov: 0, recall: 0, recallN: 0 };
    let sumHy = { hits: 0, cov: 0, recall: 0, recallN: 0 };
    for (const c of golden.cases) {
      const k = c.k ?? 15;

      // keyword モード: 運用キャッシュを破壊せず、埋め込み経路だけ明示的に無効化する。
      const kwRows = searchExternalVoiceRows(core, config.workspace, [c.theme], k, warn, {
        useEmbedding: false,
      });
      const kw = evaluateResult(kwRows, c);

      // hybrid モード: クエリ埋め込みを明示的に温めてから実行。
      let hy: ModeResult = kw;
      if (config.embedding.enabled) {
        await warmQueryEmbedding(
          core.client.raw,
          embedder,
          config.embedding.model,
          normalizeQueryText([c.theme]),
          warn
        );
        const hyRows = searchExternalVoiceRows(core, config.workspace, [c.theme], k, warn);
        hy = evaluateResult(hyRows, c);
      }

      console.log(`\n議題: ${c.theme}${c.note ? ` (${c.note})` : ""}`);
      console.log(`  keyword: ${fmt(kw)}`);
      console.log(`  hybrid : ${fmt(hy)}`);
      sumKw.hits += kw.hits;
      sumKw.cov += kw.markerCoverage;
      sumHy.hits += hy.hits;
      sumHy.cov += hy.markerCoverage;
      if (kw.recallAtK !== null) {
        sumKw.recall += kw.recallAtK;
        sumKw.recallN += 1;
      }
      if (hy.recallAtK !== null) {
        sumHy.recall += hy.recallAtK;
        sumHy.recallN += 1;
      }
    }
    const n = golden.cases.length;
    console.log(`\n=== 集計 (${n} ケース) ===`);
    console.log(
      `keyword: avgHits=${(sumKw.hits / n).toFixed(1)} avgMarkerCov=${(sumKw.cov / n).toFixed(2)}` +
        (sumKw.recallN ? ` avgRecall=${(sumKw.recall / sumKw.recallN).toFixed(2)}` : "")
    );
    console.log(
      `hybrid : avgHits=${(sumHy.hits / n).toFixed(1)} avgMarkerCov=${(sumHy.cov / n).toFixed(2)}` +
        (sumHy.recallN ? ` avgRecall=${(sumHy.recall / sumHy.recallN).toFixed(2)}` : "")
    );
  } finally {
    core.close?.();
  }
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
