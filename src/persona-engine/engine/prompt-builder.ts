/**
 * Rule fire 時の prompt 組み立て。
 *
 * 構成:
 *   system: persona の identity (name + display_name + traits + speech_style)
 *   user:   rule.instructions + 議論コンテキスト (JSON) + JSON 応答必須の指示
 *
 * 応答は JSON のみ (action + 必須フィールド)。 handler 側で zod 相当の検証。
 */

import type { PersonaRow, RuleRow } from "../types.js";
import type {
  DiscussionContextProvider,
  ContextGap,
  ContextHypothesis,
  ContextUtterance,
} from "../context-provider.js";

export interface BuildPromptArgs {
  rule: RuleRow;
  persona: PersonaRow;
  workspaceId: string;
  /** event 発火時の sessionId (tick の場合は null 可) */
  sessionId: string | null;
  contextProvider: DiscussionContextProvider;
  triggeredBy: string;
}

export interface BuiltPrompt {
  system: string;
  user: string;
  ctx: {
    hypotheses: ContextHypothesis[];
    gaps: ContextGap[];
    utterances: ContextUtterance[];
  };
}

export function buildPrompt(args: BuildPromptArgs): BuiltPrompt {
  const traits = safeJsonArrayString(args.persona.traits);
  const system = [
    `あなたは議論ペルソナ「${args.persona.name} (${args.persona.display_name})」 です。`,
    `性格 / 役割: ${args.persona.description}`,
    `特徴: ${traits.join(" / ")}`,
    `話し方: ${args.persona.speech_style}`,
    "",
    "あなたは Discutere の議論エンジン上で発言するペルソナです。",
    "",
    "【発言 (text フィールド) の書き方 — 重要】",
    "- Discord のチャットで実在の人間が話すように、 自然な口語で書く。",
    "- 「反証:」「反例:」「弱点:」のような ラベル / 見出しは絶対に付けない。",
    "  否定したい時は『それは違うんじゃない?』のように普通の言葉で。",
    "  反例を出す時は『たとえば〇〇のケースだと…』のように具体例として話す。",
    "- 箇条書きや番号は使わず、 会話の一言として書く。",
    "- 一文ごとに改行する (。! ? で改行)。",
    "- 1〜3 文程度。 長文の説明にしない。",
    "",
    "出力は **必ず JSON のみ** で、 説明文や前置きを付けないでください。",
    "【応答 JSON の形式 — action は厳密にこの名前】",
    "- 発言する時: {\"action\":\"post_utterance\",\"text\":\"<あなたの発言>\"}",
    "  ※ action は必ず \"post_utterance\"。 \"speak\" や \"say\" 等の別名は不可 (エラーになる)。",
    "- 発言しない時: {\"action\":\"skip\",\"reasoning\":\"<理由>\"}",
    "確信が無い場合や、 ルールの意図に合わない場合は躊躇なく skip を返してください。",
  ].join("\n");

  const hypotheses = args.contextProvider.listActiveHypotheses(
    args.workspaceId,
    8
  );
  const gaps = args.contextProvider.listRecentGaps(args.workspaceId, 6);
  // tick rule は sessionId を持たないので、 そのままだと直近発話が空になり
  // ペルソナが議論の流れを見られず同じ意見をループする。 進行中の議論 session を
  // 解決して直近発話を渡す (= 既出を避けられる)。
  const effectiveSessionId =
    args.sessionId ??
    args.contextProvider.findActiveDiscussionSession?.({ workspaceId: args.workspaceId }) ??
    null;
  const utterances = effectiveSessionId
    ? args.contextProvider.listRecentUtterances(args.workspaceId, effectiveSessionId, 12)
    : [];

  const ctx = { hypotheses, gaps, utterances };

  const user = [
    `# Rule (triggered by ${args.triggeredBy})`,
    `id: ${args.rule.id}`,
    args.rule.description ? `description: ${args.rule.description}` : "",
    "",
    "## 指示",
    args.rule.instructions,
    "",
    "## 議論コンテキスト (JSON)",
    "```json",
    JSON.stringify(ctx, null, 2),
    "```",
    "",
    "上記コンテキストを踏まえ、 指示に従って応答 JSON のみを返してください。",
  ]
    .filter((s) => s.length > 0)
    .join("\n");

  return { system, user, ctx };
}

function safeJsonArrayString(raw: string): string[] {
  try {
    const v = JSON.parse(raw);
    if (Array.isArray(v)) return v.filter((x) => typeof x === "string");
  } catch {
    /* ignore */
  }
  return [];
}
