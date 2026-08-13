/**
 * 外部の声 RAG の検索精度評価ハーネス。
 *
 *   npm run eval:retrieval                       # data/eval/retrieval-golden.json を評価
 *   npm run eval:retrieval -- --golden path.json # ゴールデンセット指定
 *   npm run eval:retrieval -- --judge            # LLM 関連度ジャッジも併記
 *
 * ゴールデンセット (JSON) の各ケースについて、
 *   - keyword: キーワード検索のみ (クエリ埋め込み未使用)
 *   - hybrid : クエリ埋め込みを温めてからハイブリッド検索
 * の 2 モードで検索し、次を比較する:
 *   - hits          : 返却された声のうち relevantMarkers のいずれかを含む件数
 *   - markerCoverage: relevantMarkers のうち少なくとも 1 件の声に現れた割合
 *   - recallAtK     : relevantIds 指定時のみ (正解 id の回収率)
 *   - judged*       : --judge 時のみ。LLM が議題×声の関連度を 0/1/2 で採点した
 *                     judgedRelevant (score≥1 の件数) と avgScore。marker 指標は
 *                     キーワード検索に構造的に有利 (部分文字列一致) なので、
 *                     意味検索の利得はこちらで測る。採点は config.classifier の
 *                     LLM を使い、data/eval/relevance-judgments.json にキャッシュ
 *                     (同じ モデル×議題×声 は再採点しない = コスト有界・再実行決定的)。
 *
 * 「精度が上がった」を主観でなく数字で判定するための基盤。改善前後で
 * この出力を比較する。ゴールデンセットは実 KG に合わせて育てる
 * (雛形: data/eval/retrieval-golden.example.json)。
 */

import fs from "node:fs";
import path from "node:path";

import { getConfig, type DiscutereConfig } from "../src/config.js";
import { createCore } from "../src/core/index.js";
import { resolveActiveKgPath } from "../src/core/kg-registry.js";
import { createOpenAiCompatEmbedder } from "../src/core/vectors/embedder.js";
import {
  normalizeQueryText,
  warmQueryEmbedding,
} from "../src/core/vectors/query-embed-cache.js";
import { searchExternalVoiceRows } from "../src/discatier-engine-adapter/voice-search.js";
import type { LLMClient } from "../src/persona-engine/llm/client.js";
import { AnthropicSdkClient } from "../src/persona-engine/llm/anthropic.js";
import { ClaudeCliClient } from "../src/persona-engine/llm/claude-cli.js";
import { LocalOpenAiClient } from "../src/persona-engine/llm/local-openai.js";
import {
  JudgmentCache,
  judgeRelevance,
  type RelevanceScore,
} from "./lib/relevance-judge.js";

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

interface JudgedResult {
  /** score が付いた声の数 (LLM 失敗/採点漏れは対象外)。 */
  judged: number;
  /** score >= 1 の声の数。 */
  relevant: number;
  /** judged に対する平均 score (0..2)。judged=0 なら null。 */
  avgScore: number | null;
}

function evaluateJudged(
  rows: ReadonlyArray<{ id: string }>,
  scores: ReadonlyMap<string, RelevanceScore>
): JudgedResult {
  let judged = 0;
  let relevant = 0;
  let sum = 0;
  for (const r of rows) {
    const s = scores.get(r.id);
    if (s === undefined) continue;
    judged += 1;
    sum += s;
    if (s >= 1) relevant += 1;
  }
  return { judged, relevant, avgScore: judged > 0 ? sum / judged : null };
}

function fmtJudged(r: JudgedResult): string {
  const avg = r.avgScore === null ? "-" : r.avgScore.toFixed(2);
  return `judgedRelevant=${r.relevant}/${r.judged} avgScore=${avg}`;
}

/**
 * 採点用 LLM を config.classifier から作る (buildClassifierLlm と同じ選択規則)。
 * 評価専用スクリプトなので backend=off / 前提欠落は fail-fast (§7.1 — 黙って
 * marker 指標だけに degrade しない。--judge を外せば従来評価は動く)。
 */
function buildJudgeLlm(config: DiscutereConfig): { llm: LLMClient; modelKey: string } {
  const c = config.classifier;
  if (c.backend === "off") {
    throw new Error("--judge には LLM が必要 (config.classifier.backend が off)");
  }
  if (c.backend === "anthropic") {
    const apiKey = config.llm.anthropicApiKey;
    if (!apiKey) throw new Error("--judge: classifier.backend=anthropic だが ANTHROPIC_API_KEY 無し");
    return { llm: new AnthropicSdkClient({ apiKey, defaultModel: c.model }), modelKey: c.model };
  }
  if (c.backend === "local") {
    return {
      llm: new LocalOpenAiClient({
        baseUrl: config.llm.local.baseUrl,
        defaultModel: c.model || config.llm.local.model,
        apiKey: config.llm.local.apiKey,
        defaultTimeoutMs: JUDGE_TIMEOUT_MS,
      }),
      modelKey: c.model || config.llm.local.model,
    };
  }
  return {
    llm: new ClaudeCliClient({
      defaultModel: c.model,
      defaultTimeoutMs: JUDGE_TIMEOUT_MS,
      gitBashPath: config.workerPool.gitBashPath ?? config.llm.gitBashPath,
    }),
    modelKey: c.model,
  };
}

/** 採点は 20 件一括の長めプロンプトなので classifier の短い既定より余裕を取る。 */
const JUDGE_TIMEOUT_MS = 120_000;
const JUDGMENT_CACHE_PATH = "data/eval/relevance-judgments.json";

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
  const useJudge = args.includes("--judge");
  // 前提は入口で検証する (fail-fast) — 採点用 LLM が作れないならここで止まる。
  const judge = useJudge ? buildJudgeLlm(config) : null;
  const judgmentCache = useJudge ? new JudgmentCache(path.resolve(JUDGMENT_CACHE_PATH)) : null;

  const embedder = createOpenAiCompatEmbedder(config.embedding);
  const core = createCore(resolveActiveKgPath(config));
  const warn = (m: string): void => console.warn(`[eval] ${m}`);
  try {
    let sumKw = { hits: 0, cov: 0, recall: 0, recallN: 0, jRel: 0, jScore: 0, jN: 0 };
    let sumHy = { hits: 0, cov: 0, recall: 0, recallN: 0, jRel: 0, jScore: 0, jN: 0 };
    for (const c of golden.cases) {
      const k = c.k ?? 15;

      // keyword モード: 運用キャッシュを破壊せず、埋め込み経路だけ明示的に無効化する。
      const kwRows = searchExternalVoiceRows(core, config.workspace, [c.theme], k, warn, {
        useEmbedding: false,
      });
      const kw = evaluateResult(kwRows, c);

      // hybrid モード: クエリ埋め込みを明示的に温めてから実行。
      let hyRows = kwRows;
      let hy: ModeResult = kw;
      if (config.embedding.enabled) {
        await warmQueryEmbedding(
          core.client.raw,
          embedder,
          config.embedding.model,
          normalizeQueryText([c.theme]),
          warn
        );
        hyRows = searchExternalVoiceRows(core, config.workspace, [c.theme], k, warn);
        hy = evaluateResult(hyRows, c);
      }

      console.log(`\n議題: ${c.theme}${c.note ? ` (${c.note})` : ""}`);
      console.log(`  keyword: ${fmt(kw)}`);
      console.log(`  hybrid : ${fmt(hy)}`);

      if (judge && judgmentCache) {
        // 両モードの返却集合の和集合を 1 回だけ採点する (モード間で判定を共有)。
        const byId = new Map<string, { id: string; content: string }>();
        for (const r of [...kwRows, ...hyRows]) {
          if (!byId.has(r.id)) byId.set(r.id, { id: r.id, content: r.rawContent });
        }
        const scores = await judgeRelevance({
          llm: judge.llm,
          theme: c.theme,
          items: [...byId.values()],
          cache: judgmentCache,
          cacheModelKey: judge.modelKey,
          warn,
        });
        const kwJudged = evaluateJudged(kwRows, scores);
        const hyJudged = evaluateJudged(hyRows, scores);
        console.log(`  keyword: ${fmtJudged(kwJudged)}`);
        console.log(`  hybrid : ${fmtJudged(hyJudged)}`);
        sumKw.jRel += kwJudged.relevant;
        sumHy.jRel += hyJudged.relevant;
        if (kwJudged.avgScore !== null) {
          sumKw.jScore += kwJudged.avgScore * kwJudged.judged;
          sumKw.jN += kwJudged.judged;
        }
        if (hyJudged.avgScore !== null) {
          sumHy.jScore += hyJudged.avgScore * hyJudged.judged;
          sumHy.jN += hyJudged.judged;
        }
      }

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
    const judgedPart = (s: typeof sumKw): string =>
      judge ? ` judgedRelevant=${s.jRel}${s.jN ? ` avgScore=${(s.jScore / s.jN).toFixed(2)}` : ""}` : "";
    console.log(`\n=== 集計 (${n} ケース) ===`);
    console.log(
      `keyword: avgHits=${(sumKw.hits / n).toFixed(1)} avgMarkerCov=${(sumKw.cov / n).toFixed(2)}` +
        (sumKw.recallN ? ` avgRecall=${(sumKw.recall / sumKw.recallN).toFixed(2)}` : "") +
        judgedPart(sumKw)
    );
    console.log(
      `hybrid : avgHits=${(sumHy.hits / n).toFixed(1)} avgMarkerCov=${(sumHy.cov / n).toFixed(2)}` +
        (sumHy.recallN ? ` avgRecall=${(sumHy.recall / sumHy.recallN).toFixed(2)}` : "") +
        judgedPart(sumHy)
    );
  } finally {
    core.close?.();
  }
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
