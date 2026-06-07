/**
 * 8 固定キャスト用の debate ルール seed。
 *
 * engine は `personaId = rule.target` で発火するため、target を worker id に
 * 合わせる必要がある (既存 DISCUSSION_RULE_SEEDS の target=advocate/sceptic 等は
 * worker と一致せず全 skip になる)。
 *
 * 各ワーカーは standing prompt で役割・口調・出力プロトコルが固定済なので、
 * rule.instructions は「今このターンで何をするか」の短い指示だけ持たせる。
 * ワーカーは素の発話テキストを返し、WorkerPoolClient が
 * {action:"post_utterance", text} に包んで handler に渡す。
 */

import type { RuleSeed } from "../types.js";

const SPEAK = (role: string) =>
  `直近の議論を読み、${role}として短く一言 (1〜3 文) 発言してください。` +
  `既出の繰り返しや、いま付け足す事が無ければ何も書かず skip してください (発言テキストを空にする)。`;

export const DEBATE_RULE_SEEDS: RuleSeed[] = [
  // ── 人間発言への応答 (最優先・短 cooldown) ──
  {
    id: "human-facilitator",
    description: "人間の発言にファシリテーターが即応する",
    trigger_type: "event",
    event_kind: "HumanUtterance",
    target: "facilitator",
    cooldown_sec: 15,
    instructions:
      "人間の発言を最優先で受け止め、論点を整理して次の一手を一言で投げてください。" +
      "不要なら skip。",
  },
  {
    id: "human-opinion-opus",
    description: "人間の発言に意見屋(Opus)が反応する",
    trigger_type: "event",
    event_kind: "HumanUtterance",
    target: "opinion-opus",
    cooldown_sec: 45,
    instructions: SPEAK("意見屋"),
  },

  // ── 自走 debate (tick・役割ごとに cooldown をずらして輪番) ──
  // tick_sec = engine の tick loop が発火候補に上げる間隔 (これが無いと
  // engine.ts の `if (!r.tick_sec) continue` で永久に skip され議論が始まらない)。
  // cooldown_sec と同値にして「cooldown 周期ごとに 1 回発火」を意味させる。
  { id: "tick-con-opus", description: "否定派(Opus)が反論する", trigger_type: "tick", target: "con-opus", tick_sec: 80, cooldown_sec: 80, instructions: SPEAK("否定派") },
  { id: "tick-pro-opus", description: "正論派(Opus)が擁護する", trigger_type: "tick", target: "pro-opus", tick_sec: 85, cooldown_sec: 85, instructions: SPEAK("正論派") },
  { id: "tick-con-gpt", description: "否定派(GPT)が反論する", trigger_type: "tick", target: "con-gpt", tick_sec: 110, cooldown_sec: 110, instructions: SPEAK("否定派") },
  { id: "tick-pro-gpt", description: "正論派(GPT)が擁護する", trigger_type: "tick", target: "pro-gpt", tick_sec: 115, cooldown_sec: 115, instructions: SPEAK("正論派") },
  { id: "tick-opinion-sonnet", description: "意見屋(Sonnet)が独自の角度で", trigger_type: "tick", target: "opinion-sonnet", tick_sec: 100, cooldown_sec: 100, instructions: SPEAK("意見屋") },
  { id: "tick-opinion-gpt", description: "意見屋(GPT)が独自の角度で", trigger_type: "tick", target: "opinion-gpt", tick_sec: 125, cooldown_sec: 125, instructions: SPEAK("意見屋") },
  {
    id: "tick-facilitator-steer",
    description: "ファシリテーターが論点を絞る/止揚で収束を促す",
    trigger_type: "tick",
    target: "facilitator",
    tick_sec: 150,
    cooldown_sec: 150,
    instructions:
      "議論の流れを見て、対立を止揚 (アウフヘーベン) する方向に論点を一つ投げるか、" +
      "十分まとまっていれば収束を促してください。promotしすぎず、不要なら skip。",
  },
];

/** ルール id → 既定 instructions (runtime override の比較・デフォルト用)。 */
export const RULE_INSTRUCTION_DEFAULTS: Record<string, string> = Object.fromEntries(
  DEBATE_RULE_SEEDS.map((r) => [r.id, r.instructions]),
);

/**
 * 各ルールの instructions に runtime override を適用した新しい seed 配列を返す (pure)。
 * override が無い (= undefined / 空) ルールは元の instructions を保つ。
 */
export function applyRuleInstructionOverrides(
  seeds: RuleSeed[],
  getInstruction: (ruleId: string) => string | undefined,
): RuleSeed[] {
  return seeds.map((r) => {
    const ov = getInstruction(r.id);
    return ov && ov.trim() !== "" ? { ...r, instructions: ov } : r;
  });
}
