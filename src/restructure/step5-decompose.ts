/**
 * ⑤ コア / オプション分解。
 *
 * resource グラフの次数中心性 + LLM の反実仮想判定で各 Mechanic に role を付与する。
 * spec/plan/RESTRUCTURE.md §2-⑤。
 */

import type { GameIntake, EconomyPhase0, Step5Output } from "./types.js";
import type { LlmAdapter } from "./llm-adapter.js";
import { computeEconomyCentrality } from "./step3-economy.js";

export async function decomposeMechanics(
  intake: GameIntake,
  economy: EconomyPhase0,
  llm: LlmAdapter,
): Promise<Step5Output> {
  const centrality = computeEconomyCentrality(economy);

  // LLM に判定を依頼し、次数中心性を補足情報として注入
  const economyWithCentrality: EconomyPhase0 = {
    ...economy,
    // edges に centrality メタを注記するのではなく
    // mechanics の順序を centrality 降順にして LLM ヒントにする
    mechanics: [...intake.mechanics].sort((a, b) => {
      const ca = centrality.get(a.name) ?? 0;
      const cb = centrality.get(b.name) ?? 0;
      return cb - ca;
    }),
  };

  const result = await llm.decomposeCoreOption(intake.mechanics, economyWithCentrality);

  // 次数中心性を LLM 結果に上書きマージ
  for (const role of result.roles) {
    const c = centrality.get(role.mechanicName) ?? 0;
    role.economyCentrality = c;
  }

  return result;
}
