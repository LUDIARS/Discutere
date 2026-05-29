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
    "出力は **必ず JSON のみ** で、 説明文や前置きを付けないでください。",
    "確信が無い場合や、 ルールの意図に合わない場合は躊躇なく {\"action\":\"skip\",\"reasoning\":\"<理由>\"} を返してください。",
  ].join("\n");

  const hypotheses = args.contextProvider.listActiveHypotheses(
    args.workspaceId,
    8
  );
  const gaps = args.contextProvider.listRecentGaps(args.workspaceId, 6);
  const utterances = args.sessionId
    ? args.contextProvider.listRecentUtterances(
        args.workspaceId,
        args.sessionId,
        10
      )
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
