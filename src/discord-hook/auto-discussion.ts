import type { LLMClient } from "../persona-engine/index.js";
import { createCore } from "../core/index.js";
import type { ReturnTypeCreateCore } from "../core/projection/types.js";

export type AutoDiscussionCategory =
  | "game_design_question"
  | "mechanic_question"
  | "opinion"
  | "noise"
  | "command_like";

/**
 * 議論の方向性 (フォーラムのタグで指定)。
 * - "improvement": 改善提案 — 課題を洗い出し具体的な改善案を出し合う。
 * - "fun": 面白さ — 何がどう面白いか、体験の魅力を語り合う (タグ無しの既定)。
 */
export type ForumDirection = "improvement" | "fun";

/** タグが無い / 判別不能なときの既定方向 (= 面白さ)。 */
export const DEFAULT_FORUM_DIRECTION: ForumDirection = "fun";

/** 方向性を facilitator が読む議題説明に差し込むためのディレクティブ文。 */
export function forumDirectionDirective(direction: ForumDirection): string {
  return direction === "improvement"
    ? "【議論の方向性: 改善提案】この議題はゲーム/メカニクスの課題を洗い出し、具体的な改善案を出し合って収束させる。"
    : "【議論の方向性: 面白さ】この議題は何がどう面白いのか、体験の魅力を多角的に語り合って収束させる。";
}

export interface DiscordAutoDiscussionInput {
  workspaceId: string;
  guildId: string;
  channelId: string;
  sessionId: string;
  utteranceId: string;
  authorId: string;
  content: string;
  /**
   * フォーラムスレッドのタイトル (= 議論の主題)。フォーラム starter のみ設定。
   * 本文に主題 (ゲーム名等) が書かれていなくても、 タイトルから議題を固定する。
   * これが無いと分類器が本文だけで主題を推測し、 別ゲームに誤爆する。
   */
  forumTitle?: string;
  /**
   * 議論の方向性 (フォーラムのタグ由来)。設定時のみ議題説明にディレクティブを差し込む。
   * 非フォーラム経路 (平文議論) は未設定 = 従来挙動。
   */
  direction?: ForumDirection;
}

export interface AutoDiscussionClassification {
  action: "start_discussion" | "record_only";
  category: AutoDiscussionCategory;
  title: string;
  description: string;
  /** 議題のゲームタイトル (特定できなければ undefined)。議論の主題アンカー。 */
  gameTitle?: string;
  /** 議題のジャンル (特定できなければ undefined)。 */
  genre?: string;
  expectedAffect?: string;
  observedAffect?: string;
  reason?: string;
}

export interface DiscordAutoDiscussionStarterOptions {
  getLlm?: () => LLMClient | null;
}

export function createDiscordAutoDiscussionStarter(
  options: DiscordAutoDiscussionStarterOptions = {}
): (input: DiscordAutoDiscussionInput) => Promise<{ started: boolean }> {
  return async (input) => {
    const core = createCore();
    try {
      const r = await startAutoDiscussionForDiscordMessage(core, input, options.getLlm?.() ?? null);
      // started=true は「議論の種(designGap)が新規に立った=開始エントリ」を意味する。
      // caller (gateway) はこの時だけリアクションを付ける。
      return { started: r.started };
    } finally {
      core.close();
    }
  };
}

export async function startAutoDiscussionForDiscordMessage(
  core: ReturnTypeCreateCore,
  input: DiscordAutoDiscussionInput,
  llm: LLMClient | null = null
): Promise<{ started: boolean; gapId?: string; classification: AutoDiscussionClassification }> {
  const existing = findGapByEvidenceUtterance(core, input.workspaceId, input.utteranceId);
  const classification = await classifyDiscordMessage(input.content, llm, input.forumTitle);
  if (existing) return { started: false, gapId: existing, classification };
  if (classification.action !== "start_discussion") return { started: false, classification };

  // 議題のアンカー: 対象ゲーム + ジャンル を gap タイトル/説明の先頭に固定する。
  // これが persona に渡る (ContextGap.description) ことで、議論が対象ゲームから逸れない。
  const forumTitle = input.forumTitle?.trim();
  const gameTitle = classification.gameTitle?.trim() || (forumTitle && forumTitle.length > 0 ? forumTitle : undefined);
  const genre = classification.genre?.trim();
  const gapTitle = gameTitle || forumTitle || classification.title;
  // 主題アンカー (ゲーム + ジャンル) → 投稿の要点 の順で description を組む。
  const anchorLine = gameTitle
    ? `【対象ゲーム: ${gameTitle}${genre ? ` / ジャンル: ${genre}` : ""}】` +
      `この議論は必ずこのゲーム・ジャンルに即して論じる。別ゲームの話や前提のすり替えは持ち込まない。`
    : forumTitle && forumTitle.length > 0
      ? `【主題: ${forumTitle}】`
      : "";
  const baseDescription = [anchorLine, classification.description].filter(Boolean).join("\n");

  // 方向性 (フォーラムタグ由来) があれば議題説明にディレクティブを差し込む。
  // facilitator は gapTopic (title + description) を読むので、これで拡張/収束が方向に沿う。
  const description = input.direction
    ? `${baseDescription}\n\n${forumDirectionDirective(input.direction)}`
    : baseDescription;

  const gapId = core.repos.designGap.create({
    workspaceId: input.workspaceId,
    title: gapTitle,
    description,
    status: "open",
  });

  const evidence = {
    source: "discord:auto-classifier",
    category: classification.category,
    gameTitle: gameTitle ?? null,
    genre: genre ?? null,
    reason: classification.reason ?? null,
    direction: input.direction ?? null,
    utteranceIds: [input.utteranceId],
    guildId: input.guildId,
    channelId: input.channelId,
    sessionId: input.sessionId,
    authorId: input.authorId,
  };
  core.client.raw
    .prepare(
      `UPDATE design_gaps
       SET gap_in = ?, expected_affect = ?, observed_affect = ?, evidence_json = ?, updated_at = ?
       WHERE id = ?`
    )
    .run(
      // gap_in は dedup ユニーク索引 (workspace_id, gap_in, expected_affect, observed_affect) の
      // キー。discord 議論はスレッド/投稿ごとに独立なので gapId を含めて一意化する。
      // (`discord:<category>` だけだと同カテゴリ+同 affect の 2 件目で UNIQUE 制約違反 →
      //  classify 失敗でクラッシュしていた。category は evidence_json.category に保持。)
      `discord:${classification.category}:${gapId}`,
      classification.expectedAffect ?? null,
      classification.observedAffect ?? null,
      JSON.stringify(evidence),
      Date.now(),
      gapId
    );

  return { started: true, gapId, classification };
}

export async function classifyDiscordMessage(
  content: string,
  llm: LLMClient | null = null,
  threadTitle?: string
): Promise<AutoDiscussionClassification> {
  const title = threadTitle?.trim();
  const fallback = classifyDiscordMessageFallback(content, title);
  if (!llm) return fallback;

  const result = await llm.invoke({
    system:
      "You classify Discord posts for an automatic game-design discussion system. " +
      "You MUST read the post body carefully and identify which game and genre it is about. Return only compact JSON.",
    prompt: [
      "Classify the post.",
      // スレッドタイトルは議論の主題 (対象ゲーム等)。本文に主題が無くてもこれを基準にする。
      title ? `Thread title (議論の主題・対象ゲーム — これが対象ゲームの最優先根拠): ${title}` : "",
      "",
      "【ゲームタイトル・ジャンルの判定 (最重要)】",
      "- 投稿本文(とスレッドタイトル)から、議論の対象となる『ゲームタイトル』と『ジャンル』を特定する。",
      "- gameTitle: 具体的なゲーム名 (例「Vampire Survivors」「ワンダと巨像」)。特定できなければ空文字。",
      "- genre: そのゲームのジャンル (例「ローグライト」「アクションアドベンチャー」「ヴァンサバ系」)。",
      "- 整合性を判断する: genre がその gameTitle に実際に合っているか? 投稿が本当にその対象についてか?",
      "  矛盾する / どのゲーム・ジャンルか特定できない 場合は action=record_only にし、reason に理由を書く。",
      "  (対象ゲーム・ジャンルが定まらない議題は噛み合わないので議論を開始しない。)",
      "- start_discussion にするのは、gameTitle が特定でき、genre と整合し、議論に値する内容のときだけ。",
      "",
      "JSON schema:",
      '{"action":"start_discussion|record_only","category":"game_design_question|mechanic_question|opinion|noise|command_like","gameTitle":"対象ゲーム名 or 空","genre":"ジャンル or 空","title":"short Japanese title","description":"本文の要点を踏まえた日本語要約","expectedAffect":"optional","observedAffect":"optional","reason":"short reason (特に開始しない場合は理由)"}',
      "",
      `Post: ${content}`,
    ]
      .filter((l) => l.length > 0)
      .join("\n"),
    maxTokens: 700,
  });

  if (!result.ok) return fallback;
  return normalizeClassification(parseClassificationJson(result.text) ?? fallback, content, title, true);
}

export function classifyDiscordMessageFallback(
  content: string,
  threadTitle?: string
): AutoDiscussionClassification {
  const text = content.trim();
  const lower = text.toLowerCase();
  const isQuestion = /[?？]|とは|なに|何|なぜ|どう|必要|要素|改善|課題|問題|面白|ゲーム|プレイ|ヴァンサバ|vampire/.test(
    text
  );
  if (text.startsWith("/") || lower.startsWith("http")) {
    return normalizeClassification({
      action: "record_only",
      category: "command_like",
      title: "",
      description: "",
      reason: "command-like or link-only content",
    }, text, threadTitle);
  }
  if (!isQuestion || text.length < 6) {
    return normalizeClassification({
      action: "record_only",
      category: "noise",
      title: "",
      description: "",
      reason: "not enough discussion signal",
    }, text, threadTitle);
  }
  // フォールバックでも gameTitle はスレッドタイトルから採る (正規化側で threadTitle を採用)。
  // gameTitle が取れない (= 非フォーラム平文) と record_only に降格する。
  return normalizeClassification({
    action: "start_discussion",
    category: /要素|改善|課題|問題|面白|ゲーム|プレイ|ヴァンサバ|vampire/.test(text)
      ? "game_design_question"
      : "mechanic_question",
    title: `Discord議題: ${text}`,
    description: `Discord投稿から自動検出: ${text}`,
    expectedAffect: "unknown",
    observedAffect: "question",
    reason: "question or game-design terms detected",
  }, text, threadTitle);
}

function parseClassificationJson(text: string): Partial<AutoDiscussionClassification> | null {
  const trimmed = text.trim();
  const match = trimmed.match(/\{[\s\S]*\}/);
  if (!match) return null;
  try {
    const parsed = JSON.parse(match[0]);
    return parsed && typeof parsed === "object" ? parsed : null;
  } catch {
    return null;
  }
}

function normalizeClassification(
  raw: Partial<AutoDiscussionClassification>,
  content: string,
  threadTitle?: string,
  // LLM 判定時のみ true。LLM はゲーム名を本文から特定できるので、特定できない=議題が
  // 噛み合わないと判断し record_only に降格する。regex fallback はゲーム名を抽出できない
  // ので降格しない (非フォーラム平文議論を従来どおり通す)。
  enforceGameTitle = false
): AutoDiscussionClassification {
  const category = normalizeCategory(raw.category);
  const title = truncate((raw.title?.trim() || `Discord議題: ${content.trim()}`), 80);
  const description = truncate(
    raw.description?.trim() || `Discord投稿から自動検出: ${content.trim()}`,
    600
  );
  // スレッドタイトルは対象ゲームの最優先根拠。LLM が gameTitle を返さなければ title を採用。
  const title0 = threadTitle?.trim();
  const gameTitle = raw.gameTitle?.trim() || title0 || undefined;
  const genre = raw.genre?.trim() || undefined;
  // 対象ゲームが特定できない議題は噛み合わないので議論を開始しない (record_only に降格)。
  // FEATURE ⑤(a): ただし threadTitle (フォーラムスレッド題 / 種まきの主題) が非空なら
  //   それを主題アンカーに使えるので降格しない (= eyes 無し投稿でも種にできる)。
  //   threadTitle が無い平文チャンネルの LLM 経路は従来どおり降格を維持 (ノイズ防止)。
  const wantsStart = raw.action === "start_discussion";
  const downgrade = enforceGameTitle && wantsStart && !gameTitle && !title0;
  const action: AutoDiscussionClassification["action"] =
    wantsStart && !downgrade ? "start_discussion" : "record_only";
  const reason = downgrade
    ? `対象ゲームを特定できないため議論を開始しない (${raw.reason?.trim() ?? "gameTitle 不明"})`
    : raw.reason?.trim() || undefined;
  return {
    action,
    category,
    title,
    description,
    gameTitle,
    genre,
    expectedAffect: raw.expectedAffect?.trim() || undefined,
    observedAffect: raw.observedAffect?.trim() || undefined,
    reason,
  };
}

function normalizeCategory(category: unknown): AutoDiscussionCategory {
  return category === "game_design_question" ||
    category === "mechanic_question" ||
    category === "opinion" ||
    category === "noise" ||
    category === "command_like"
    ? category
    : "noise";
}

function truncate(value: string, max: number): string {
  return value.length <= max ? value : `${value.slice(0, max - 1)}…`;
}

function findGapByEvidenceUtterance(
  core: ReturnTypeCreateCore,
  workspaceId: string,
  utteranceId: string
): string | undefined {
  const rows = core.client.raw
    .prepare("SELECT id, evidence_json FROM design_gaps WHERE workspace_id = ? AND evidence_json IS NOT NULL")
    .all(workspaceId) as Array<{ id: string; evidence_json: string | null }>;
  for (const row of rows) {
    if (!row.evidence_json) continue;
    try {
      const evidence = JSON.parse(row.evidence_json) as { utteranceIds?: unknown };
      if (Array.isArray(evidence.utteranceIds) && evidence.utteranceIds.includes(utteranceId)) {
        return row.id;
      }
    } catch {
      continue;
    }
  }
  return undefined;
}
