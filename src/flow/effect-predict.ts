/**
 * 効果予測 (改善フロー スコアリング再設計 / improvement.md (2) 2026-07-02 改訂, respec 07)。
 *
 * 旧方式 `textToVector(意見文)` は「意見の文面に表出した感情」を測るもので、
 * 「意見を採用した場合の体験変化」ではない (カテゴリエラー / 指摘 A3)。
 * ここでは小モデル LLM に「この意見を採用した場合、体験の 20 次元がどう動くか」を
 * 構造化 JSON (変化次元 + delta -1..1) で予測させ、その予測変化ベクトルを返す。
 * 射影・20 次元空間 (VECTOR_DIMS / SENTIMENT.md) は不変。
 *
 * LLM 失敗 / パース失敗は null を返し、呼び出し側 (improvement.ts) が意見単位で
 * 旧 lexicon 方式へフォールバックする (improvement_score.method に記録 + warn)。
 * DB 非依存・LLM 注入 — 単体テスト可能。
 */

import type { LLMClient } from "../persona-engine/llm/client.js";
import type { MechanicSummary } from "./investigate.js";
import { DIM, VECTOR_DIMS } from "./sentiment-vector.js";

/** 予測された 1 次元の変化。dim は VECTOR_DIMS の次元名、delta は -1..1。 */
export interface EffectChange {
  dim: string;
  delta: number;
  reason?: string;
}

/** 効果予測の結果 (構造化 JSON のパース済み形)。 */
export interface EffectPrediction {
  /** 変化しそうな次元だけ (言及のない次元は移動なし = 0)。 */
  changes: EffectChange[];
  confidence: "high" | "low";
}

export interface PredictEffectArgs {
  /** 採点対象の意見テキスト。 */
  opinion: string;
  theme: string;
  /** ペーパーのメカニクス (予測の文脈として要約して渡す)。 */
  mechanics: MechanicSummary[];
  llm: LLMClient;
  /** 効果予測モデル (config flow.improvement.effectModel)。"" / 未指定なら LLM 既定。 */
  model?: string;
  warn?: (msg: string) => void;
}

/** プロンプトに載せるメカニクスの最大件数 (文脈用の要約)。 */
const MECHANIC_SAMPLE = 10;
/** 予測 JSON 用の出力トークン上限 (changes は高々 20 次元 + reason)。 */
const MAX_TOKENS = 1500;

const DIM_INDEX = new Map(VECTOR_DIMS.map((d, i) => [d, i]));

/**
 * 文字列から最初の JSON オブジェクトを取り出す (コードフェンス/前置き混入に耐える)。
 * まず最初の `{` 〜 最後の `}` の全体パース、失敗したらブレースマッチで先頭オブジェクトを救済。
 */
export function extractJsonObject(raw: string): Record<string, unknown> | null {
  const start = raw.indexOf("{");
  if (start < 0) return null;
  const end = raw.lastIndexOf("}");
  if (end > start) {
    try {
      const parsed = JSON.parse(raw.slice(start, end + 1)) as unknown;
      if (parsed && typeof parsed === "object" && !Array.isArray(parsed)) {
        return parsed as Record<string, unknown>;
      }
    } catch {
      // 救済へ
    }
  }
  // 救済: 先頭の {...} をブレースマッチ (文字列内ブレースは無視) で切り出して個別パース。
  let depth = 0;
  let inStr = false;
  let esc = false;
  for (let i = start; i < raw.length; i++) {
    const ch = raw[i];
    if (inStr) {
      if (esc) esc = false;
      else if (ch === "\\") esc = true;
      else if (ch === '"') inStr = false;
      continue;
    }
    if (ch === '"') inStr = true;
    else if (ch === "{") depth++;
    else if (ch === "}") {
      depth--;
      if (depth === 0) {
        try {
          const parsed = JSON.parse(raw.slice(start, i + 1)) as unknown;
          if (parsed && typeof parsed === "object" && !Array.isArray(parsed)) {
            return parsed as Record<string, unknown>;
          }
        } catch {
          // 壊れていたら諦める
        }
        return null;
      }
    }
  }
  return null;
}

const clamp = (v: number, lo: number, hi: number): number => Math.max(lo, Math.min(hi, v));

/**
 * パース済み JSON を EffectPrediction に正規化する。
 * 未知の次元名は捨てる (warn)、delta は -1..1 にクランプ。changes が配列でなければ null。
 */
export function normalizePrediction(
  raw: Record<string, unknown>,
  warn: (msg: string) => void = () => {}
): EffectPrediction | null {
  if (!Array.isArray(raw.changes)) return null;
  const changes: EffectChange[] = [];
  for (const c of raw.changes) {
    if (!c || typeof c !== "object") continue;
    const item = c as Record<string, unknown>;
    const dim = typeof item.dim === "string" ? item.dim.trim() : "";
    const delta = typeof item.delta === "number" ? item.delta : NaN;
    if (!dim || !Number.isFinite(delta)) continue;
    if (!DIM_INDEX.has(dim)) {
      warn(`効果予測: 未知の次元名 "${dim}" を無視 (VECTOR_DIMS 外)`);
      continue;
    }
    changes.push({
      dim,
      delta: clamp(delta, -1, 1),
      ...(typeof item.reason === "string" ? { reason: item.reason } : {}),
    });
  }
  const confidence = raw.confidence === "low" ? "low" : "high";
  return { changes, confidence };
}

/**
 * 予測変化 (言及次元のみ) を 20 次元の移動ベクトルにする。
 * 言及のない次元は 0 (移動なし)。同一次元の重複は後勝ち。各次元 -1..1。
 */
export function changesToVector(changes: readonly EffectChange[]): number[] {
  const v = new Array<number>(DIM).fill(0);
  for (const c of changes) {
    const idx = DIM_INDEX.get(c.dim);
    if (idx === undefined) continue;
    v[idx] = clamp(c.delta, -1, 1);
  }
  return v;
}

function buildPrompt(args: PredictEffectArgs): { system: string; prompt: string } {
  const mechanicsText =
    args.mechanics.length > 0
      ? args.mechanics
          .slice(0, MECHANIC_SAMPLE)
          .map((m) => `- ${m.name}: ${m.description.slice(0, 120)}`)
          .join("\n")
      : "(なし)";
  const system =
    "あなたはゲームデザイン分析者です。改善議論で出た意見を **採用した場合に、プレイヤー体験が" +
    "どう変化するか** を予測します。意見の文面の感情ではなく、採用後の体験の変化を評価してください" +
    " (熱く前向きな文面でも、採用すると体験が悪化する提案は負の変化です)。返答は JSON オブジェクトのみ。";
  const prompt =
    `# テーマ\n${args.theme}\n\n# 既知のメカニクス (文脈)\n${mechanicsText}\n\n` +
    `# 体験の次元 (この名前だけを使う)\n${VECTOR_DIMS.join(", ")}\n\n` +
    `# 意見\n${args.opinion}\n\n` +
    "この意見を採用した場合に **変化しそうな次元だけ** を挙げ、変化量 delta (-1..1、負=悪化/正=改善) を" +
    "予測してください。全次元を無理に挙げないこと。次の JSON オブジェクトだけで返してください" +
    " (前後に説明やコードフェンスを付けない):\n" +
    '{"changes":[{"dim":"asp.fun","delta":0.4,"reason":"変化の理由 (1文)"}],"confidence":"high または low"}';
  return { system, prompt };
}

/**
 * 意見の採用効果を LLM で予測する。
 * LLM 失敗 / JSON パース失敗 / changes 欠落は null (呼び出し側が lexicon 方式へ fallback)。
 */
export async function predictEffect(args: PredictEffectArgs): Promise<EffectPrediction | null> {
  const warn = args.warn ?? (() => {});
  let res;
  try {
    const { system, prompt } = buildPrompt(args);
    res = await args.llm.invoke({
      system,
      prompt,
      ...(args.model ? { model: args.model } : {}),
      maxTokens: MAX_TOKENS,
    });
  } catch (e) {
    warn(`効果予測で例外: ${(e as Error).message}`);
    return null;
  }
  if (!res.ok) {
    warn(`効果予測に失敗: ${res.error}`);
    return null;
  }
  const obj = extractJsonObject(res.text);
  if (!obj) {
    warn("効果予測の JSON 解析に失敗");
    return null;
  }
  const prediction = normalizePrediction(obj, warn);
  if (!prediction) {
    warn("効果予測の changes が不正 (配列でない)");
    return null;
  }
  return prediction;
}
