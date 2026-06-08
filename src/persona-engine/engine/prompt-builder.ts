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
  ContextExternalVoice,
  ContextGap,
  ContextHypothesis,
  ContextUtterance,
} from "../context-provider.js";

/** prompt に載せる外部の声の最大件数 (RAG / §14)。 粗く動かす段階の既定。 */
const EXTERNAL_VOICE_TOP_K = 6;

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
    /** 議題に関連する外部の生の声 (RAG / §14)。 出所付き・個人は仮名。 無ければ省略。 */
    externalVoices?: ContextExternalVoice[];
  };
}

/**
 * 議題 (gap title + description) から検索語を粗く抽出する (RAG retrieval 用)。
 * 日本語は分かち書きできないので、 記号区切りの 2 文字以上トークン + タイトル全体を語にする
 * (adapter 側は substring 一致なのでこれで機能する)。
 */
function extractTopicTerms(gap: ContextGap): string[] {
  const text = `${gap.title} ${gap.description ?? ""}`;
  const tokens = text
    .split(/[\s、。,.!?！？「」『』（）()\/:：;；・\-—\n\r\t]+/)
    .map((t) => t.trim())
    .filter((t) => t.length >= 2);
  return Array.from(new Set([gap.title.trim(), ...tokens])).filter((t) => t.length >= 2);
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

  // 議題アンカー: 直近の open gap の title + description (対象ゲーム/ジャンル + 投稿の要点) を
  // 最上部に明示する。これを外すと persona が gap.description を読まず議論が主題から逸れる。
  const primaryGap =
    gaps.find((g) => !["closed", "dismissed", "converged", "resolved"].includes((g.status || "").toLowerCase())) ??
    gaps[0];

  // 外部の声 (RAG / §14): 議題語に関連する実在の外部意見を active KG から取得して注入し、
  // persona が「実際の声」を出所付きで引用・参照できるようにする。 retrieval 未実装なら空。
  const externalVoices =
    primaryGap && args.contextProvider.listRelevantExternalVoices
      ? args.contextProvider.listRelevantExternalVoices(
          args.workspaceId,
          extractTopicTerms(primaryGap),
          EXTERNAL_VOICE_TOP_K
        )
      : [];

  const ctx = {
    hypotheses,
    gaps,
    utterances,
    ...(externalVoices.length > 0 ? { externalVoices } : {}),
  };
  const topicBlock = primaryGap
    ? [
        "## 議題 (この前提から絶対に外れない)",
        `題目: ${primaryGap.title}`,
        primaryGap.description ? primaryGap.description : "",
        "→ 上の対象ゲーム・ジャンルに即して論じる。別ゲームや別ジャンルの話に逸らさない。",
        "",
      ]
    : [];

  const voiceBlock =
    externalVoices.length > 0
      ? [
          "## 外部の声 (JSON の externalVoices)",
          "externalVoices は外部から集めた実在の意見 (出所付き / 投稿者は仮名)。",
          "議論に関連するものは「○○ (source) はこう言ってる」のように**出所(ソース種別)を添えて引用・参照**してよい。",
          "投稿者個人の特定はしない (表示名は仮名のまま)。関係ない声は無視してよい。",
          "",
        ]
      : [];

  const user = [
    `# Rule (triggered by ${args.triggeredBy})`,
    `id: ${args.rule.id}`,
    args.rule.description ? `description: ${args.rule.description}` : "",
    "",
    ...topicBlock,
    ...voiceBlock,
    "## 指示",
    args.rule.instructions,
    "",
    "## これまでの発言 (utterances は時系列。人間の発言があれば最優先で踏まえ、無視しない)",
    "## 議論コンテキスト (JSON)",
    "```json",
    JSON.stringify(ctx, null, 2),
    "```",
    "",
    "上記の議題と発言を必ず踏まえ、 指示に従って応答 JSON のみを返してください。",
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
