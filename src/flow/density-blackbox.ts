/**
 * 情報密度判定の成長型ブラックボックス化 (@ludiars/blackbox)。
 *
 * evaluateInformationDensity (毎回 LLM 1 コール) を包み、判定を特徴量
 * (voiceCount / sourceKinds / avgLen / tagCount) ベースのルールへ蒸留する。
 * LLM が提案した閾値ルールは candidate として影評価で信頼を積み、
 * trial 発火 → admin の OK×3 で auto 卒業 = 以後この密度判定に LLM を呼ばない。
 *
 * ルール経路では gaps (不足観点クエリ) は返せないが、runInformationGate は
 * gaps 空のとき theme をクエリにクロールするので degrade で回る。
 * 設計正本: Lapilli packages/blackbox/DESIGN.md §7。
 */

import type Database from "better-sqlite3";
import {
  makeSqliteBlackBox, validateCondition,
  type BlackBox, type FeatureMap, type LlmJudgement,
} from "@ludiars/blackbox";
import {
  evaluateInformationDensity,
  isDensity,
  type EvaluateArgs,
  type InformationDensity,
  type InformationEvaluation,
} from "./information-gate.js";
import type { ContextVoice } from "./discussion-paper.js";
import type { FlowTag } from "./tags.js";

export const DOMAIN_DENSITY = "flow.information_density";

interface DensityOutput {
  density: InformationDensity;
}

/** 密度判定をルール化するためのフラット特徴量。 */
export function densityFeatures(voices: ContextVoice[], tags: readonly FlowTag[]): FeatureMap {
  const kinds = new Set(voices.map((v) => v.source));
  const totalLen = voices.reduce((acc, v) => acc + v.content.length, 0);
  return {
    voiceCount: voices.length,
    sourceKinds: kinds.size,
    avgLen: voices.length === 0 ? 0 : Math.round(totalLen / voices.length),
    tagCount: tags.length,
  };
}

/** LLM 評価の proposedRule (raw) を検証して blackbox 提案形式に直す。 */
function parseProposedRule(
  raw: unknown,
  density: InformationDensity,
): LlmJudgement<DensityOutput>["proposedRule"] {
  if (!raw || typeof raw !== "object") return undefined;
  const r = raw as Record<string, unknown>;
  try {
    const when = validateCondition(r.when);
    return {
      description: typeof r.description === "string" ? r.description : `密度 ${density} の閾値ルール`,
      when,
      output: { density },
      confidence: typeof r.confidence === "number" ? r.confidence : 0.7,
    };
  } catch {
    return undefined;
  }
}

/** flow DB (discutere.db) 上の blackbox を束ねる。 */
export function makeDensityBlackBox(db: Database.Database): BlackBox {
  return makeSqliteBlackBox(db);
}

/**
 * evaluateInformationDensity 互換の評価器を作る。
 * 卒業/試用ルールが features に一致すれば LLM を呼ばず即決 (gaps は空 = theme クエリで degrade)。
 * ルールが無ければ従来どおり LLM 評価し、その判定を教師に candidate を育てる。
 */
export function makeBlackboxDensityEvaluator(
  bb: BlackBox,
  evaluateFn: typeof evaluateInformationDensity = evaluateInformationDensity,
): (args: EvaluateArgs) => Promise<InformationEvaluation> {
  return async (args: EvaluateArgs): Promise<InformationEvaluation> => {
    const features = densityFeatures(args.voices, args.tags);
    let llmEvaluation: InformationEvaluation | null = null;

    const { decision } = await bb.engine.decide<{ theme: string }, DensityOutput>(
      DOMAIN_DENSITY,
      { theme: args.theme },
      features,
      async () => {
        const ev = await evaluateFn(args);
        llmEvaluation = ev;
        return {
          output: { density: ev.density },
          confidence: 0.7,
          rationale: `covered ${ev.covered.length} / gaps ${ev.gaps.length}`,
          proposedRule: parseProposedRule(ev.proposedRule, ev.density),
        };
      },
    );

    // LLM 経路: gaps/covered 込みの完全な評価をそのまま返す。
    if (llmEvaluation) return llmEvaluation;

    // ルール経路: 密度のみ。判定が不正な output を持っていたら安全側 (sparse=学習継続) に倒す。
    const density = isDensity(decision.output?.density) ? decision.output.density : "sparse";
    return { density, covered: [], gaps: [] };
  };
}
