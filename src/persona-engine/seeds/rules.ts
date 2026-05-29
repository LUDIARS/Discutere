/**
 * 議論駆動 rule seed (Phase 0)。
 *
 * 議論を「自走」 させるための最小ルール 4 種。 すべて cooldown 付きで
 * 連発を防ぐ。 trigger:
 *   - tick: 一定間隔で発火 (= 議論の自発的進行)
 *   - event: 議論側の event (GapDetected / HypothesisValidated 等) で発火
 *
 * instructions は LLM への「主指示」 で、 prompt-builder が
 * persona の speech_style + 議論コンテキストを合体して投げる。
 *
 * 「行動を返さない (skip)」 も valid な選択肢。 過剰発話を防ぐため。
 */

import type { RuleSeed } from "../types.js";

export const DISCUSSION_RULE_SEEDS: RuleSeed[] = [
  {
    id: "propose-on-gap",
    description:
      "design gap が検出されたら、 advocate (推進派) が新規 hypothesis を提案する",
    trigger_type: "event",
    event_kind: "DesignGapDetected",
    target: "advocate",
    cooldown_sec: 60,
    instructions: `あなたは議論における推進派です。 直前に検出された design gap を読み、
新規 hypothesis (statement: 短い 1 文) を提案してください。
既存の hypothesis と被らないことを最優先で確認し、 被るなら "skip" を返してください。

返答は JSON のみ:
{
  "action": "propose_hypothesis",
  "statement": "<提案する仮説の 1 文>",
  "reasoning": "<なぜこれを提案するか、 30 字以内>"
}
or
{
  "action": "skip",
  "reasoning": "<理由>"
}`,
  },
  {
    id: "refute-cold",
    description:
      "5 分以上反論されていない hypothesis を sceptic が突く",
    trigger_type: "tick",
    tick_sec: 300,
    target: "sceptic",
    cooldown_sec: 180,
    instructions: `あなたは議論における懐疑者です。 active な hypothesis のうち、
直近 5 分以上 反論 utterance が無いものを 1 つ選び、 弱点を 1 文で指摘してください。
弱点が見つからない場合は "skip" を返してください。

返答は JSON のみ:
{
  "action": "post_utterance",
  "hypothesis_id": "<対象 hypothesis id>",
  "text": "<弱点指摘の 1 文 (鋭く短く)>",
  "reasoning": "<なぜこれを突くか、 30 字以内>"
}
or { "action": "skip", "reasoning": "..." }`,
  },
  {
    id: "refine-validated",
    description:
      "validated になった hypothesis を refiner が条件付き精緻化する",
    trigger_type: "event",
    event_kind: "HypothesisValidated",
    target: "refiner",
    cooldown_sec: 120,
    instructions: `あなたは議論における慎重派です。 直前に validated された hypothesis を読み、
条件を絞った精緻化版の hypothesis を提案してください。
全く同じ意味なら "skip" を返してください。

返答は JSON のみ:
{
  "action": "propose_hypothesis",
  "statement": "<条件付き精緻化版の 1 文>",
  "addresses_gap_id": "<元 hypothesis が取り組む gap id (継承)>",
  "reasoning": "<どこを絞ったか、 30 字以内>"
}
or { "action": "skip", "reasoning": "..." }`,
  },
  {
    id: "integrate-on-many",
    description:
      "同じ gap に 3 件以上 hypothesis が並んだら integrator が統合提案する",
    trigger_type: "tick",
    tick_sec: 600,
    target: "integrator",
    cooldown_sec: 600,
    instructions: `あなたは議論における統合者です。 同じ design gap に紐づく hypothesis が
3 件以上ある場合、 それらの共通項を取り出して上位概念化した
1 件の統合 hypothesis を提案してください。 3 件未満の gap しか
無い場合は "skip" を返してください。

返答は JSON のみ:
{
  "action": "propose_hypothesis",
  "statement": "<統合 hypothesis の 1 文>",
  "addresses_gap_id": "<対象 gap id>",
  "supersedes": ["<束ねた hypothesis id>", "..."],
  "reasoning": "<統合の根拠、 50 字以内>"
}
or { "action": "skip", "reasoning": "..." }`,
  },
];
