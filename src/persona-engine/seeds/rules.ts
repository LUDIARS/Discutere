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
    id: "respond-to-human",
    description:
      "人間の発言には最優先かつ即座に応答する (人間 > AI、 遠慮不要)",
    trigger_type: "event",
    event_kind: "HumanUtterance",
    target: "advocate",
    cooldown_sec: 8,
    instructions: `議論チャンネルに **人間の参加者** が発言しました。
議論コンテキスト (utterances) の **最新 (末尾) の発言が その人間のもの** です。
あなたはその人間の発言に **最優先で即座に** 応答してください。

- AI 同士の進行中のやり取りよりも、 人間の発言を優先する。
- 遠慮や過剰な配慮は不要。 賛否・具体例・反論を率直に述べてよい (馴れ合わない)。
- Discord の会話として自然な口語で 1〜2 文、 一文ごとに改行。
- ただし 人間が「議論のテーマ自体を変えたい / 別の話を始めたい」 ように見える場合は、
  あなたが勝手に話題や場を切り替えず "skip" を返す
  (場を変えるかどうかの判断はファシリテーターが行う)。

返答は JSON のみ:
{ "action": "post_utterance", "text": "<人間への応答 1〜2 文>", "reasoning": "<30字以内>" }
or { "action": "skip", "reasoning": "場を変える発言 / 応答不要などの理由" }`,
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
  {
    id: "meta-improve-rules",
    description:
      "中立観察者が直近の議論状況を見て、 必要なら rule を 1 つ追加 / 削除する (= 議論エンジンの自己補正)",
    trigger_type: "tick",
    tick_sec: 1800,
    target: "neutral-observer",
    cooldown_sec: 1800,
    instructions: `あなたは議論における中立観察者です。 直近の rule fire 状況 / hypothesis の
進行を俯瞰し、 議論が停滞 / 偏向 / 暴走している場合に rule を 1 つだけ
**追加** または **削除** することで改善を試みてください。
変更が不要なら "skip" を返してください。

制約:
- 既存 seed rule (propose-on-gap / refute-cold / refine-validated /
  integrate-on-many / meta-improve-rules) は **削除不可**
- 新規 rule の instructions には "沈黙確認" "進捗確認" "汎用雑談" 系の
  禁則を含めない (システムが reject する)
- 1 度の応答で 1 アクションのみ

返答は JSON のみ。

(A) 新規 rule 追加:
{
  "action": "add_rule",
  "rule": {
    "id": "<kebab-case 一意 id>",
    "description": "<短い説明>",
    "trigger_type": "tick" | "event",
    "tick_sec": <5-3600、 trigger=tick の時>,
    "event_kind": "<DesignGapDetected | HypothesisValidated | ... >",
    "target": "<persona id (advocate / sceptic / refiner / ...)>",
    "cooldown_sec": <0-86400、 default 60>,
    "instructions": "<persona に投げる主指示、 JSON 応答形式を必ず含む>"
  },
  "reasoning": "<なぜこの rule を追加するか>"
}

(B) 既存 rule 削除:
{
  "action": "remove_rule",
  "rule_id": "<既存 rule id>",
  "reasoning": "<なぜ削除するか>"
}

(C) 何もしない:
{ "action": "skip", "reasoning": "..." }`,
  },
];

/** seed rule は AI による削除から保護する (= remove_rule で拒否) */
export const SEED_RULE_IDS = new Set<string>([
  "propose-on-gap",
  "respond-to-human",
  "refute-cold",
  "refine-validated",
  "integrate-on-many",
  "meta-improve-rules",
]);
