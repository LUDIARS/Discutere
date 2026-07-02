/**
 * 情報ゲート — 議論開始前の情報密度評価フェーズ。
 *
 * 旧来の autoCrawl は「外部の声が minVoices 件未満ならクロール」という単純カウント閾値で、
 * 情報の密度・観点の偏りを見ていなかった (= 情報が薄いまま議論が始まり精度が落ちる)。
 * ここは議論/改善フロー開始の直前に:
 *   1. テーマの既存材料 (収集済みの外部の声) を集め、
 *   2. LLM で情報密度 (sparse/moderate/rich) と不足観点 (gaps) を評価し、
 *   3. 不足していれば不足観点を狙ったクエリでクロール → KG 取込し、
 *   4. 再評価する (十分 or 反復上限 or 密度が伸びるまで)。
 *
 * 自動モード固定: 不足なら自動で学習し、十分になり次第そのまま議論を開始する。
 * LLM 障害時は議論を止めない (件数ベースの粗い fallback で degrade)。
 *
 * この module は LLMClient を直接呼ぶ (コストログのラップは呼び出し側 = information-gate-runner)。
 * DB に触れないので単体テスト可能。
 */

import type { LLMClient } from "../persona-engine/llm/client.js";
import type { createCore } from "../core/index.js";
import type { ContextVoice } from "./discussion-paper.js";
import type { FlowTag } from "./tags.js";
import { extractJsonObject } from "../persona-engine/facilitator/prompts.js";
import {
  collectAndImport,
  deriveSlug,
  isAutoCrawlSource,
  type AutoCrawlSource,
  type AutoCrawlSpec,
} from "./learning-autocrawl.js";

type Core = ReturnType<typeof createCore>;

/** 情報密度の段階 (sparse < moderate < rich)。 */
export type InformationDensity = "sparse" | "moderate" | "rich";

const DENSITY_RANK: Record<InformationDensity, number> = { sparse: 0, moderate: 1, rich: 2 };

export function isDensity(s: unknown): s is InformationDensity {
  return s === "sparse" || s === "moderate" || s === "rich";
}

/** 評価時に密度判定に使う外部の声の取得上限。 */
const ASSESS_LIMIT = 50;

export interface InformationGap {
  /** 不足している観点。 */
  aspect: string;
  /** なぜその観点が必要か。 */
  reason: string;
  /** 不足を埋めるためのクロール検索クエリ。 */
  query: string;
  /** LLM が推奨するソース (advisory; 実クロールは config 既定ソースを使う)。 */
  source?: AutoCrawlSource;
}

export interface InformationEvaluation {
  density: InformationDensity;
  /** 既にカバーできている観点。 */
  covered: string[];
  /** 不足している観点とその学習クエリ。 */
  gaps: InformationGap[];
  /**
   * LLM が「この密度判定は閾値ルールで再現できる」と考えた場合の Condition 候補 (raw)。
   * 解釈・検証は density-blackbox 側 (validateCondition) で行う。
   */
  proposedRule?: unknown;
}

/** 件数だけから粗く密度を見積もる (LLM 評価が使えない時の fallback)。 */
function fallbackDensity(voices: ContextVoice[]): InformationDensity {
  if (voices.length >= 8) return "rich";
  if (voices.length >= 3) return "moderate";
  return "sparse";
}

function fallbackEvaluation(voices: ContextVoice[]): InformationEvaluation {
  return { density: fallbackDensity(voices), covered: [], gaps: [] };
}

function normalizeGaps(raw: unknown[], theme: string): InformationGap[] {
  const out: InformationGap[] = [];
  for (const g of raw) {
    if (!g || typeof g !== "object") continue;
    const o = g as Record<string, unknown>;
    const aspect = typeof o.aspect === "string" ? o.aspect.trim() : "";
    if (!aspect) continue;
    const reason = typeof o.reason === "string" ? o.reason.trim() : "";
    const queryRaw = typeof o.query === "string" ? o.query.trim() : "";
    const query = queryRaw || `${theme} ${aspect}`.trim();
    const source = typeof o.source === "string" && isAutoCrawlSource(o.source) ? o.source : undefined;
    out.push({ aspect, reason, query, source });
  }
  return out;
}

function buildMaterialText(theme: string, tags: readonly FlowTag[], voices: ContextVoice[]): string {
  const tagText = tags.length ? tags.join(" / ") : "(なし)";
  if (voices.length === 0) {
    return `議題: ${theme}\n観点タグ: ${tagText}\n\n収集済みの外部の声: (0 件 — 材料なし)`;
  }
  const lines = voices
    .slice(0, 30)
    .map(
      (v, i) =>
        `${i + 1}. [${v.source}] ${v.content.slice(0, 160)}${v.content.length > 160 ? "…" : ""}`
    );
  return `議題: ${theme}\n観点タグ: ${tagText}\n\n収集済みの外部の声 (${voices.length} 件):\n${lines.join("\n")}`;
}

function buildEvalPrompt(material: string): { system: string; prompt: string } {
  const system =
    "あなたは議論の準備担当です。 与えられた議題と、 収集済みの『外部の声』(プレイヤー/視聴者の生の感想) を見て、" +
    " この議題で深い議論をするのに情報が足りているかを評価します。 量だけでなく、 観点の多様性" +
    " (肯定/否定/具体例/異なる立場) も評価軸にしてください。 返答は JSON のみ。";
  const prompt =
    `${material}\n\n` +
    "次の JSON だけを返してください (前後に説明文を付けない):\n" +
    "{\n" +
    '  "density": "sparse" | "moderate" | "rich",\n' +
    '  "covered": ["既にカバーできている観点", ...],\n' +
    '  "gaps": [\n' +
    '    { "aspect": "不足している観点", "reason": "なぜ必要か", "query": "それを集める検索クエリ(日本語可)", "source": "niconico|youtube|steam|website" }\n' +
    "  ],\n" +
    '  "proposedRule": { "description": "…", "when": { "op": "and", "clauses": [ { "op": "cmp", "feature": "voiceCount", "cmp": ">=", "value": 8 }, { "op": "cmp", "feature": "sourceKinds", "cmp": ">=", "value": 2 } ] } }\n' +
    "}\n" +
    "声が少ない / 観点が偏る (例: 肯定だけ・具体例なし) なら density=sparse とし、" +
    " gaps に不足観点と検索クエリを 1〜3 個挙げてください。 十分多様で厚いなら rich とし gaps は空配列で構いません。\n" +
    "proposedRule は任意です: この密度判定が単純な閾値で再現できるなら、" +
    " feature 名 voiceCount (件数) / sourceKinds (ソース種類数) / avgLen (平均文字数) / tagCount を使った" +
    " Condition を書いてください。自信が無ければ省略してください。";
  return { system, prompt };
}

export interface EvaluateArgs {
  theme: string;
  tags: readonly FlowTag[];
  voices: ContextVoice[];
  llm: LLMClient;
  model?: string;
  warn?: (msg: string) => void;
}

/**
 * 収集済み材料から情報密度と不足観点を 1 回評価する (LLM 1 コール)。
 * LLM 失敗 / JSON 解析失敗は件数ベースの fallback に倒す (議論を止めない)。
 */
export async function evaluateInformationDensity(args: EvaluateArgs): Promise<InformationEvaluation> {
  const { theme, tags, voices, llm, model, warn = () => {} } = args;
  const { system, prompt } = buildEvalPrompt(buildMaterialText(theme, tags, voices));

  let res;
  try {
    res = await llm.invoke({ system, prompt, model });
  } catch (e) {
    warn(`情報密度評価で例外: ${(e as Error).message}`);
    return fallbackEvaluation(voices);
  }
  if (!res.ok) {
    warn(`情報密度評価に失敗: ${res.error}`);
    return fallbackEvaluation(voices);
  }
  try {
    const parsed = extractJsonObject(res.text) as Record<string, unknown>;
    const density = isDensity(parsed.density) ? parsed.density : fallbackDensity(voices);
    const covered = Array.isArray(parsed.covered)
      ? parsed.covered.filter((c): c is string => typeof c === "string")
      : [];
    const gaps = Array.isArray(parsed.gaps) ? normalizeGaps(parsed.gaps, theme) : [];
    return { density, covered, gaps, proposedRule: parsed.proposedRule };
  } catch (e) {
    warn(`情報密度評価の JSON 解析に失敗: ${(e as Error).message}`);
    return fallbackEvaluation(voices);
  }
}

/** ゲートの動作設定 (config.flow.informationGating)。 */
export interface InformationGateConfig {
  enabled: boolean;
  /** これ以上の密度なら「十分」とみなす閾値。 */
  minDensity: InformationDensity;
  /** 不足時に学習 (クロール) を回す最大反復回数。 */
  maxLearnIterations: number;
  /** 1 反復で狙い撃ちする不足観点の最大数 (クロール回数 = コスト上限)。 */
  maxGapsPerIteration: number;
  /** 評価に使うモデル (未指定なら LLM の既定)。 */
  model?: string;
}

export interface InformationGateDeps {
  core: Core;
  theme: string;
  tags: readonly FlowTag[];
  workspaceId: string;
  /** 評価用 LLM (呼び出し側で withCostLog 済みを渡す)。 */
  llm: LLMClient;
  /** 議論と同じ外部の声検索 (= 密度評価の材料)。 未指定なら材料 0 件扱い。 */
  listExternalVoices?: (terms: string[], limit: number) => ContextVoice[];
  /** クロールに使う既定ソース (後方互換の単数指定)。crawlSources 未指定時のフォールバック。 */
  crawlSource?: AutoCrawlSource;
  /** クロールに使うソース群 (複数ソース横断学習)。未指定なら [crawlSource]。 */
  crawlSources?: AutoCrawlSource[];
  maxItems?: number;
  youtubeApiKey?: string;
  config: InformationGateConfig;
  /** 取得→取込の差し替え (テスト用)。 既定は learning-autocrawl.collectAndImport。 */
  collectAndImportFn?: typeof collectAndImport;
  /**
   * 密度評価の差し替え。 既定は evaluateInformationDensity (毎回 LLM)。
   * runner は density-blackbox でラップした評価器を渡し、卒業ルールで LLM を省く。
   */
  evaluateFn?: typeof evaluateInformationDensity;
  log?: (msg: string) => void;
  warn?: (msg: string) => void;
}

export interface InformationGateResult {
  /** ゲートを実行したか (= enabled で対象フロー)。 */
  ran: boolean;
  /** 最終的に閾値を満たしたか。 */
  sufficient: boolean;
  initialDensity: InformationDensity;
  finalDensity: InformationDensity;
  /** クロール反復回数。 */
  crawls: number;
  /** 取込総件数。 */
  importedTotal: number;
  /** 実際にクロールした検索クエリ。 */
  crawledQueries: string[];
  message: string;
}

/**
 * 情報ゲートを実行する。 不足なら不足観点を狙って学習 → 再評価し、 閾値に届くか反復上限まで回す。
 * 届かなくても議論は開始できるよう sufficient=false で返す (呼び出し側が degrade して続行)。
 */
export async function runInformationGate(deps: InformationGateDeps): Promise<InformationGateResult> {
  const {
    core,
    theme,
    tags,
    workspaceId,
    llm,
    listExternalVoices,
    maxItems = 200,
    youtubeApiKey,
    config,
    log = () => {},
    warn = () => {},
  } = deps;
  const doCollect = deps.collectAndImportFn ?? collectAndImport;
  const doEvaluate = deps.evaluateFn ?? evaluateInformationDensity;
  // クロール対象ソース: crawlSources 優先、無ければ単数 crawlSource にフォールバック。
  const crawlSources: AutoCrawlSource[] =
    deps.crawlSources && deps.crawlSources.length > 0
      ? deps.crawlSources
      : deps.crawlSource
        ? [deps.crawlSource]
        : [];

  const listVoices = (): ContextVoice[] => {
    if (!listExternalVoices) return [];
    try {
      return listExternalVoices([theme], ASSESS_LIMIT);
    } catch {
      return [];
    }
  };

  const target = DENSITY_RANK[config.minDensity];
  const crawledQueries: string[] = [];
  let importedTotal = 0;
  let crawls = 0;

  let voices = listVoices();
  let evaluation = await doEvaluate({ theme, tags, voices, llm, model: config.model, warn });
  const initialDensity = evaluation.density;

  while (DENSITY_RANK[evaluation.density] < target && crawls < config.maxLearnIterations) {
    const gaps = evaluation.gaps.slice(0, Math.max(1, config.maxGapsPerIteration));
    const queries = gaps.length > 0 ? gaps.map((g) => g.query) : [theme];
    log(
      `情報密度 ${evaluation.density} < ${config.minDensity} → 不足観点 ${queries.length} 件を学習` +
        ` (反復 ${crawls + 1}/${config.maxLearnIterations})`
    );
    for (const query of queries) {
      // 各不足観点を全ソース横断で集める (niconico + youtube 等)。
      for (const source of crawlSources) {
        const spec: AutoCrawlSpec = { source, query };
        try {
          const r = await doCollect({
            core,
            theme,
            slug: deriveSlug(theme),
            workspaceId,
            spec,
            maxItems,
            youtubeApiKey,
            log,
            warn,
          });
          importedTotal += r.imported;
          crawledQueries.push(`${source}:${query}`);
        } catch (e) {
          warn(`学習クロール失敗 (source=${source} query="${query}"): ${(e as Error).message}`);
        }
      }
    }
    crawls++;
    voices = listVoices();
    evaluation = await doEvaluate({ theme, tags, voices, llm, model: config.model, warn });
  }

  const sufficient = DENSITY_RANK[evaluation.density] >= target;
  const message = sufficient
    ? `情報充分 (密度 ${initialDensity}→${evaluation.density} ≥ ${config.minDensity} / 学習 ${crawls} 回・取込 ${importedTotal} 件)`
    : `目標 (${config.minDensity}) に未達 (密度 ${initialDensity}→${evaluation.density} / 学習 ${crawls} 回・取込 ${importedTotal} 件) — このまま議論を開始`;
  log(message);

  return {
    ran: true,
    sufficient,
    initialDensity,
    finalDensity: evaluation.density,
    crawls,
    importedTotal,
    crawledQueries,
    message,
  };
}
