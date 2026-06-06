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
  const classification = await classifyDiscordMessage(input.content, llm);
  if (existing) return { started: false, gapId: existing, classification };
  if (classification.action !== "start_discussion") return { started: false, classification };

  // 方向性 (フォーラムタグ由来) があれば議題説明にディレクティブを差し込む。
  // facilitator は gapTopic (title + description) を読むので、これで拡張/収束が方向に沿う。
  const description = input.direction
    ? `${classification.description}\n\n${forumDirectionDirective(input.direction)}`
    : classification.description;

  const gapId = core.repos.designGap.create({
    workspaceId: input.workspaceId,
    title: classification.title,
    description,
    status: "open",
  });

  const evidence = {
    source: "discord:auto-classifier",
    category: classification.category,
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
      `discord:${classification.category}`,
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
  llm: LLMClient | null = null
): Promise<AutoDiscussionClassification> {
  const fallback = classifyDiscordMessageFallback(content);
  if (!llm) return fallback;

  const result = await llm.invoke({
    system:
      "You classify Discord posts for an automatic game-design discussion system. Return only compact JSON.",
    prompt: [
      "Classify the post. Start a discussion only when it is a substantive game/design/mechanic question or a debatable opinion.",
      "JSON schema:",
      '{"action":"start_discussion|record_only","category":"game_design_question|mechanic_question|opinion|noise|command_like","title":"short Japanese title","description":"Japanese summary","expectedAffect":"optional","observedAffect":"optional","reason":"short reason"}',
      `Post: ${content}`,
    ].join("\n"),
    maxTokens: 700,
  });

  if (!result.ok) return fallback;
  return normalizeClassification(parseClassificationJson(result.text) ?? fallback, content);
}

export function classifyDiscordMessageFallback(content: string): AutoDiscussionClassification {
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
    }, text);
  }
  if (!isQuestion || text.length < 6) {
    return normalizeClassification({
      action: "record_only",
      category: "noise",
      title: "",
      description: "",
      reason: "not enough discussion signal",
    }, text);
  }
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
  }, text);
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
  content: string
): AutoDiscussionClassification {
  const action = raw.action === "start_discussion" ? "start_discussion" : "record_only";
  const category = normalizeCategory(raw.category);
  const title = truncate((raw.title?.trim() || `Discord議題: ${content.trim()}`), 80);
  const description = truncate(
    raw.description?.trim() || `Discord投稿から自動検出: ${content.trim()}`,
    600
  );
  return {
    action,
    category,
    title,
    description,
    expectedAffect: raw.expectedAffect?.trim() || undefined,
    observedAffect: raw.observedAffect?.trim() || undefined,
    reason: raw.reason?.trim() || undefined,
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
