/** Fictional values; these are conditional models, not observed game data. */
import type { MechanicsCheckRequest, TextLogicDifference } from "../../src/mechanics-check/contracts.js";

export function requestFixture(): MechanicsCheckRequest {
  return {
    specText: "狙いは技能で予測可能な得点。成功で得点を得る。ボーナスはランダム。",
    baselineText: "狙いは技能で予測可能な得点。ボーナスはない。",
    references: [{ id: "criterion", source: "Elegantia fictional supplied criterion", revision: "fictional-v1", text: "仕様、観測、模型仮定を区別する。", evidenceKind: "specification" }],
    playModel: {
      metric: { id: "score", name: "得点", unit: "points", direction: "higher" },
      events: [
        { id: "success", description: "技能による成功", determinant: "player_skill", evidenceRefs: ["spec"] },
        { id: "bonus", description: "ランダムボーナス", determinant: "random", evidenceRefs: ["spec"] },
      ],
      scenarios: [{
        id: "novice", description: "初級者の一回の挑戦", skillBand: "初級者",
        assumptions: { information: "ルール既知", initialState: "得点0", trialCount: 1 },
        predictedBaseline: { value: 60, unit: "points" }, exhaustive: true,
        outcomes: [
          { id: "low", eventIds: [], value: 20, unit: "points", probability: 0.25 },
          { id: "middle", eventIds: ["success"], value: 60, unit: "points", probability: 0.5 },
          { id: "high", eventIds: ["success", "bonus"], value: 100, unit: "points", probability: 0.25 },
        ],
      }],
    },
  };
}
export function differenceFixture(): TextLogicDifference {
  return {
    kind: "revision", target: "bonus",
    left: { text: "旧版にボーナスはない。", evidence: [{ referenceId: "baseline", quote: "ボーナスはない。" }] },
    right: { text: "新版にランダムボーナスがある。", evidence: [{ referenceId: "spec", quote: "ボーナスはランダム。" }] },
    status: "inconsistent", conditions: ["入力された旧版と新版のルールの比較。"], unknowns: [],
  };
}
export function responseFixture(): string {
  return JSON.stringify({ logic_differences: [differenceFixture()], design_gap: [] });
}
