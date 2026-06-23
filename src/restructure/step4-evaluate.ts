/**
 * ④ マクロ / ミクロ評価。
 *
 * ミクロ: 個々の Mechanic を嗜好プロファイルとの適合 + LLM で評価。
 * マクロ: M 全体 + 内部経済 + 感情曲線を LLM で評価。
 * ネガティブ評価は designGap の種として negativeGapCandidates に出す。
 *
 * spec/plan/RESTRUCTURE.md §2-④ / §4-B / §4-F。
 */

import type {
  GameIntake,
  EconomyPhase0,
  PreferenceProfile,
  Step4Output,
} from "./types.js";
import type { LlmAdapter } from "./llm-adapter.js";
import { findProfile, cosine } from "./step1-preferences.js";

export async function evaluateGame(
  intake: GameIntake,
  economy: EconomyPhase0,
  profiles: PreferenceProfile[],
  llm: LlmAdapter,
): Promise<Step4Output> {
  const genreProfile = findProfile(profiles, `genre:${intake.genre ?? ""}`);
  const preferenceScore = genreProfile
    ? cosine(genreProfile.vector, genreProfile.vector) // self-cosine = 1.0 placeholder
    : 0.5;

  const [micro, macro] = await Promise.all([
    llm.evaluateMechanics(intake.mechanics, intake.genre ?? "unknown", preferenceScore),
    llm.evaluateMacro(intake.slug, intake.mechanics, economy),
  ]);

  // 嗜好スコアを mechanic ごとに補正
  for (const ev of micro) {
    const mechProfile = findProfile(profiles, `mechanic:${ev.mechanicName}`);
    if (mechProfile && genreProfile) {
      ev.preferenceCompatibility = cosine(mechProfile.vector, genreProfile.vector);
      // threshold 0.4 以下かつ嗜好不一致なら negative 扱いを維持
      ev.isNegative = ev.score < 0.4 || ev.preferenceCompatibility < 0.3;
    }
  }

  const negativeGapCandidates = [
    ...micro.filter((ev) => ev.isNegative).map((ev) => ev.mechanicName),
    ...(macro.economyBalance === "inflationary" || macro.economyBalance === "deflationary" || macro.economyBalance === "deadend"
      ? [`economy:${macro.economyBalance}`]
      : []),
  ];

  return { micro, macro, negativeGapCandidates };
}
