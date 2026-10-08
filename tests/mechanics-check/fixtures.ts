/** Fictional values; these are conditional models, not observed game data. */
import type { MechanicsCheckRequest, TextLogicDifference } from "../../src/mechanics-check/contracts.js";

export function requestFixture(): MechanicsCheckRequest {
  return {
    specText: "狙いは技能で予測可能な得点。成功で得点を得る。ボーナスはランダム。",
    baselineText: "狙いは技能で予測可能な得点。ボーナスはない。",
    references: [{ id: "criterion", source: "Elegantia fictional supplied criterion", revision: "fictional-v1", text: "仕様、観測、模型仮定を区別する。", evidenceKind: "specification" }],
  };
}
export function differenceFixture(): TextLogicDifference {
  return {
    kind: "revision", target: "bonus",
    left: { text: "旧版にボーナスはない。", evidence: [{ referenceId: "baseline", quote: "ボーナスはない。" }] },
    right: { text: "新版にランダムボーナスがある。", evidence: [{ referenceId: "spec", quote: "ボーナスはランダム。" }] },
    status: "inconsistent", causeDomain: "mechanics", conditions: ["入力された旧版と新版のルールの比較。"], unknowns: [],
  };
}
export function responseFixture(): string {
  return JSON.stringify({ logic_differences: [differenceFixture()], design_gap: [] });
}
