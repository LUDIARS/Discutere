/**
 * 議論適性ゲート (09-paper-gate-debatability) — 情報ゲート (量) の後段の「質」チェック。
 *
 * 情報が rich でも議論が成立しないテーマ (争点がない・片側しか武装できない・証拠が偏る)
 * を議論開始前に検出し、フロー種別の再提案 (壁打ち/学習) を出す。3 検査:
 *   1. 争点の存在      — decomposeIssues (dialectic [0] と同一関数、LLM)
 *   2. 両論武装可能性  — issue ごとに pro/con の両方が根拠を張れるか (LLM 小モデル判定)
 *   3. 証拠バランス    — 機械計算のみ (textToVector の valence 極性分布 + source 偏り)
 *
 * 判定: armable=both の issue が minArmableIssues (既定 2) 未満 → 議論不適。
 * 議論不適でも人間が最終決定する (強行可)。LLM 失敗はゲートを止めず degraded=true で
 * 「適性あり扱い」に倒す (ゲート失敗で議論を止めない、degrade は明示する)。
 *
 * DB 非依存・依存はすべて注入 (LLM / decompose / toVector) — 単体テスト可能。
 * コストログ (withCostLog) のラップは呼び出し側 (information-gate-runner) が行う。
 */

import type { LLMClient } from "../persona-engine/llm/client.js";
import { decomposeIssues as defaultDecomposeIssues, ISSUES_MAX } from "./dialectic/issues.js";
import { extractJsonArray } from "./mechanic-extract.js";
import { textToVector as defaultTextToVector } from "./sentiment-vector.js";

/** issue の両論武装可能性の判定値。 */
export type IssueArmable = "both" | "pro-only" | "con-only" | "neither";

export interface IssueArmability {
  issue: string;
  armable: IssueArmable;
}

/** 証拠バランス (機械計算のみ・LLM 不使用)。 */
export interface EvidenceBalance {
  /** 評価に使った外部の声の件数。 */
  voiceCount: number;
  positive: number;
  negative: number;
  neutral: number;
  /** 極性の偏り -1..1 (正 = 肯定偏重 / 負 = 否定偏重)。(pos-neg)/(pos+neg)。 */
  polaritySkew: number;
  /** 最多出所とそのシェア (0..1)。声が無ければ null。 */
  dominantSource: { name: string; share: number } | null;
}

/** フロー再提案 (議論不適時)。 */
export interface FlowRecommendation {
  flow: "sparring" | "learning";
  reason: string;
}

export interface DebatabilityResult {
  /** 分解された論点 (0〜5 個、素の論点文)。 */
  issues: string[];
  /** issue ごとの両論武装可能性。 */
  armability: IssueArmability[];
  /** armable=both の件数。 */
  armableBothCount: number;
  /** 判定に使った閾値。 */
  minArmableIssues: number;
  evidence: EvidenceBalance;
  /** 議論適性 (false = 議論不適 → 再提案)。 */
  debatable: boolean;
  /** LLM 失敗で判定を degrade したか (true のとき debatable=true に倒す)。 */
  degraded: boolean;
  /** 議論不適時のフロー再提案 (debatable=true なら null)。 */
  recommendation: FlowRecommendation | null;
  /** 人間向けの一言 (ゲート表示 / ログ)。 */
  message: string;
}

/** 証拠バランス評価に使う声の上限 (機械計算なので多めでも安い)。 */
const EVIDENCE_LIMIT = 50;
/** 「材料不足」とみなす声の件数閾値 / 極性偏り閾値。 */
const SPARSE_VOICES = 5;
const SKEW_THRESHOLD = 0.7;
/** valence の中立帯 (これ未満の絶対値は中立)。 */
const VALENCE_NEUTRAL = 0.05;
/** 両論武装判定のプロンプトに載せる声のサンプル数 / 1 件の切り詰め長。 */
const ARMABILITY_VOICE_SAMPLE = 20;
const VOICE_TRIM = 160;

export interface DebatabilityVoice {
  content: string;
  source?: string;
}

/**
 * 証拠バランスを機械計算する (LLM 不使用)。
 * 極性は既存 20 次元ベクトル (textToVector) の valence 次元 (dims[0] = (valence+1)/2) から復元する。
 */
export function computeEvidenceBalance(
  voices: readonly DebatabilityVoice[],
  toVector: (text: string) => number[] = defaultTextToVector
): EvidenceBalance {
  const sample = voices.slice(0, EVIDENCE_LIMIT);
  let positive = 0;
  let negative = 0;
  let neutral = 0;
  const bySource = new Map<string, number>();
  for (const v of sample) {
    const valence = toVector(v.content)[0] * 2 - 1;
    if (valence > VALENCE_NEUTRAL) positive++;
    else if (valence < -VALENCE_NEUTRAL) negative++;
    else neutral++;
    const src = (v.source ?? "unknown").trim() || "unknown";
    bySource.set(src, (bySource.get(src) ?? 0) + 1);
  }
  const polarized = positive + negative;
  const polaritySkew = polarized > 0 ? (positive - negative) / polarized : 0;
  let dominantSource: EvidenceBalance["dominantSource"] = null;
  for (const [name, count] of bySource) {
    const share = count / sample.length;
    if (!dominantSource || share > dominantSource.share) dominantSource = { name, share };
  }
  return {
    voiceCount: sample.length,
    positive,
    negative,
    neutral,
    polaritySkew: +polaritySkew.toFixed(4),
    dominantSource: dominantSource
      ? { name: dominantSource.name, share: +dominantSource.share.toFixed(4) }
      : null,
  };
}

function isArmable(v: unknown): v is IssueArmable {
  return v === "both" || v === "pro-only" || v === "con-only" || v === "neither";
}

export interface JudgeArmabilityArgs {
  theme: string;
  issues: readonly string[];
  voices: readonly DebatabilityVoice[];
  llm: LLMClient;
  /** 小モデル判定に使うモデル (未指定なら LLM 既定)。 */
  model?: string;
  warn?: (msg: string) => void;
}

/**
 * issue ごとの両論武装可能性を LLM (小モデル) で判定する (1 コールにバッチ)。
 * LLM 失敗 / 解析不能は null を返す (呼び出し側が degrade を明示する)。
 */
export async function judgeIssueArmability(args: JudgeArmabilityArgs): Promise<IssueArmability[] | null> {
  const { theme, issues, llm, model, warn = () => {} } = args;
  if (issues.length === 0) return [];

  const issueText = issues.map((s, i) => `${i + 1}. ${s}`).join("\n");
  const voiceText =
    args.voices.length > 0
      ? args.voices
          .slice(0, ARMABILITY_VOICE_SAMPLE)
          .map((v, i) => `${i + 1}. ${v.content.slice(0, VOICE_TRIM)}`)
          .join("\n")
      : "(材料なし)";
  const system =
    "あなたは議論の準備担当です。 各論点について、 手元の材料 (プレイヤーの声) で" +
    " 賛成側 (pro) と反対側 (con) の両方が根拠を張れるかを判定します。 返答は JSON 配列のみ。";
  const prompt =
    `# 議題\n${theme}\n\n# 論点\n${issueText}\n\n# 手元の材料 (外部の声)\n${voiceText}\n\n` +
    "各論点について、この材料で pro / con のどちら側が根拠を持てるかを判定し、" +
    "次の JSON 配列だけを返してください (index は論点番号 1 始まり):\n" +
    '[{"index":1,"armable":"both"|"pro-only"|"con-only"|"neither"}]';

  let res;
  try {
    res = await llm.invoke({ system, prompt, model });
  } catch (e) {
    warn(`両論武装判定で例外: ${(e as Error).message}`);
    return null;
  }
  if (!res.ok) {
    warn(`両論武装判定に失敗: ${res.error}`);
    return null;
  }
  const arr = extractJsonArray(res.text);
  if (!arr) {
    warn("両論武装判定の JSON 解析に失敗");
    return null;
  }
  const byIndex = new Map<number, IssueArmable>();
  for (const raw of arr) {
    if (!raw || typeof raw !== "object") continue;
    const o = raw as Record<string, unknown>;
    const idx = typeof o.index === "number" ? o.index : Number(o.index);
    if (!Number.isFinite(idx) || !isArmable(o.armable)) continue;
    byIndex.set(Math.trunc(idx), o.armable);
  }
  if (byIndex.size === 0) {
    warn("両論武装判定の結果が空 (解析不能)");
    return null;
  }
  // 判定が欠けた issue は保守的に neither (両論を確認できていない) とする。
  return issues.map((issue, i) => ({ issue, armable: byIndex.get(i + 1) ?? "neither" }));
}

export interface AssessDebatabilityArgs {
  theme: string;
  /** ペーパー本文 md (論点分解の文脈)。 */
  paperMd?: string;
  /** 収集済みの外部の声 (証拠バランス + 両論武装判定の材料)。 */
  voices: readonly DebatabilityVoice[];
  /** 判定用 LLM (呼び出し側で withCostLog 済みを渡す)。 */
  llm: LLMClient;
  /** armable=both がこれ未満 → 議論不適。既定 2 (config.flow.debatability.minArmableIssues)。 */
  minArmableIssues: number;
  model?: string;
  /** 論点分解の差し替え (テスト用 / dialectic [0] と共用)。 */
  decompose?: typeof defaultDecomposeIssues;
  /** 極性ベクトルの差し替え (テスト用)。 */
  toVector?: (text: string) => number[];
  warn?: (msg: string) => void;
}

/** 議論不適時のフロー再提案を決める (09 §2)。 */
function recommendFlow(
  issues: readonly string[],
  evidence: EvidenceBalance
): FlowRecommendation {
  const materialDeficient =
    evidence.voiceCount < SPARSE_VOICES || Math.abs(evidence.polaritySkew) >= SKEW_THRESHOLD;
  if (materialDeficient) {
    const lacking =
      evidence.polaritySkew >= SKEW_THRESHOLD
        ? "否定・批判側"
        : evidence.polaritySkew <= -SKEW_THRESHOLD
          ? "肯定・支持側"
          : "多様な観点";
    return {
      flow: "learning",
      reason:
        `材料不足が主因です (声 ${evidence.voiceCount} 件 / 極性偏り ${evidence.polaritySkew.toFixed(2)})。` +
        `${lacking}の材料を「学習」で集めてから議論すると両論が立ちます。`,
    };
  }
  return {
    flow: "sparring",
    reason: `賛否が本気で割れる争点が ${issues.length} 件と少ないため、「壁打ち」で論点を練るのが向いています。`,
  };
}

/**
 * 議論適性を評価する (3 検査 + 判定 + 再提案)。
 * LLM 失敗時はゲートで議論を止めず degraded=true で「適性あり扱い」に倒す (warn で明示)。
 */
export async function assessDebatability(args: AssessDebatabilityArgs): Promise<DebatabilityResult> {
  const { theme, paperMd, voices, llm, minArmableIssues, model, warn = () => {} } = args;
  const decompose = args.decompose ?? defaultDecomposeIssues;

  // [3] 証拠バランス (機械計算・LLM 不使用) — LLM 失敗時も必ず得られる。
  const evidence = computeEvidenceBalance(voices, args.toVector);

  const degradedResult = (why: string): DebatabilityResult => ({
    issues: [],
    armability: [],
    armableBothCount: 0,
    minArmableIssues,
    evidence,
    debatable: true,
    degraded: true,
    recommendation: null,
    message: `議論適性ゲートを実行できませんでした (${why})。適性あり扱いで続行します。`,
  });

  // [1] 争点の存在 (decomposeIssues = dialectic [0] と同一関数)。
  let issues: string[];
  try {
    issues = await decompose({ theme, paperMd, llm, model, maxIssues: ISSUES_MAX, warn });
  } catch (e) {
    warn(`議論適性ゲート degrade: ${(e as Error).message}`);
    return degradedResult("論点分解に失敗");
  }

  // [2] 両論武装可能性 (LLM 小モデル判定)。
  const armability = await judgeIssueArmability({ theme, issues, voices, llm, model, warn });
  if (armability === null) {
    warn("議論適性ゲート degrade: 両論武装判定に失敗");
    return { ...degradedResult("両論武装判定に失敗"), issues };
  }

  const armableBothCount = armability.filter((a) => a.armable === "both").length;
  const debatable = armableBothCount >= minArmableIssues;
  const recommendation = debatable ? null : recommendFlow(issues, evidence);
  const skewNote =
    Math.abs(evidence.polaritySkew) >= SKEW_THRESHOLD
      ? ` / 極性偏り ${evidence.polaritySkew.toFixed(2)}`
      : "";
  const message = debatable
    ? `議論適性: あり (両論武装可能な争点 ${armableBothCount}/${issues.length} 件${skewNote})`
    : `議論適性: 低 (両論武装可能な争点 ${armableBothCount}/${issues.length} 件 < ${minArmableIssues}${skewNote})`;

  return {
    issues,
    armability,
    armableBothCount,
    minArmableIssues,
    evidence,
    debatable,
    degraded: false,
    recommendation,
    message,
  };
}

/** issue の armable 判定を md 注記にする (`(両論: ○)` — ペーパー `# 論点` 節の表示用)。 */
export function armableAnnotation(armable: IssueArmable): string {
  switch (armable) {
    case "both":
      return "(両論: ○)";
    case "pro-only":
      return "(両論: 賛成側のみ)";
    case "con-only":
      return "(両論: 反対側のみ)";
    case "neither":
      return "(両論: ×)";
  }
}

/** DebatabilityResult から `# 論点` 節に載せる注記付き論点行を作る。 */
export function annotatedIssues(result: DebatabilityResult): string[] {
  const byIssue = new Map(result.armability.map((a) => [a.issue, a.armable]));
  return result.issues.map((issue) => {
    const armable = byIssue.get(issue);
    return armable ? `${issue} ${armableAnnotation(armable)}` : issue;
  });
}
