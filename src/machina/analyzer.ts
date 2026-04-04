/**
 * M3 MACHINA: テキスト解析エンジン
 *
 * Slack/Discord のメッセージテキストを構文解析し、
 * タスク生成の必要性・優先度・アサイン・納期を判定する。
 *
 * 将来的に Claude Haiku API による高精度な解析を追加可能。
 * 現在はルールベースの解析を実装。
 */

import type { TaskPriority } from "../shared/constants.js";
import {
  COMPLETION_KEYWORDS,
  URGENCY_KEYWORDS,
} from "../shared/constants.js";

export interface AnalysisInput {
  text: string;
  authorId?: string;
  authorName?: string;
  mentions?: string[];
  platform: "slack" | "discord";
}

export interface AnalysisResult {
  shouldCreateTask: boolean;
  shouldUpdateExisting: boolean;
  title: string;
  description: string | null;
  priority: TaskPriority;
  assigneeHint: string | null;
  dueDateHint: string | null;
  confidence: number;
  reasoning: string;
  isCompletion: boolean;
}

/** タスク生成を示唆するパターン */
const TASK_PATTERNS = [
  /(?:タスク|task)[:：]?\s*(.+)/i,
  /(?:TODO|todo|ToDo)[:：]?\s*(.+)/i,
  /(?:お願い|おねがい|頼む|頼みます)[:：]?\s*(.+)/i,
  /(.+?)(?:をお願い|を頼む|してください|して欲しい|してほしい|を作って|を修正して|を対応して)/,
  /(?:やること|やるべき|必要)[:：]?\s*(.+)/i,
  /(?:issue|イシュー|バグ|bug|不具合)[:：]?\s*(.+)/i,
  /(?:実装|implement|fix|修正|追加|add|作成|create)[:：]?\s*(.+)/i,
];

/** コマンドパターン: !task や /task で明示的にタスク作成 */
const COMMAND_PATTERNS = [
  /^[!/](?:task|タスク)\s+(.+)/i,
  /^[!/](?:machina)\s+(.+)/i,
];

/** 納期を示唆するパターン */
const DUE_DATE_PATTERNS: Array<{ pattern: RegExp; resolver: (match: RegExpMatchArray) => string | null }> = [
  {
    pattern: /(?:今日中|本日中|today)/i,
    resolver: () => {
      const d = new Date();
      d.setHours(23, 59, 0, 0);
      return d.toISOString();
    },
  },
  {
    pattern: /(?:明日まで|明日中|tomorrow)/i,
    resolver: () => {
      const d = new Date();
      d.setDate(d.getDate() + 1);
      d.setHours(23, 59, 0, 0);
      return d.toISOString();
    },
  },
  {
    pattern: /(\d{1,2})[/\-月](\d{1,2})(?:日)?(?:まで)?/,
    resolver: (match: RegExpMatchArray) => {
      const month = parseInt(match[1], 10);
      const day = parseInt(match[2], 10);
      if (month < 1 || month > 12 || day < 1 || day > 31) return null;
      const d = new Date();
      d.setMonth(month - 1, day);
      d.setHours(23, 59, 0, 0);
      if (d < new Date()) d.setFullYear(d.getFullYear() + 1);
      return d.toISOString();
    },
  },
  {
    pattern: /(\d+)\s*(?:日後|日以内|days?)/i,
    resolver: (match: RegExpMatchArray) => {
      const days = parseInt(match[1], 10);
      if (days <= 0 || days > 365) return null;
      const d = new Date();
      d.setDate(d.getDate() + days);
      d.setHours(23, 59, 0, 0);
      return d.toISOString();
    },
  },
  {
    pattern: /(?:今週中|this\s*week)/i,
    resolver: () => {
      const d = new Date();
      const dayOfWeek = d.getDay();
      const daysUntilFriday = dayOfWeek <= 5 ? 5 - dayOfWeek : 0;
      d.setDate(d.getDate() + daysUntilFriday);
      d.setHours(23, 59, 0, 0);
      return d.toISOString();
    },
  },
  {
    pattern: /(?:来週まで|来週中|next\s*week)/i,
    resolver: () => {
      const d = new Date();
      const dayOfWeek = d.getDay();
      const daysUntilNextFriday = dayOfWeek <= 5 ? 12 - dayOfWeek : 5;
      d.setDate(d.getDate() + daysUntilNextFriday);
      d.setHours(23, 59, 0, 0);
      return d.toISOString();
    },
  },
];

/**
 * メッセージテキストを解析し、タスク生成の判定を行う
 */
export function analyzeMessage(input: AnalysisInput): AnalysisResult {
  const { text, mentions, authorName } = input;
  const normalizedText = text.trim();

  // コマンドによる明示的タスク作成
  for (const pattern of COMMAND_PATTERNS) {
    const match = normalizedText.match(pattern);
    if (match) {
      return {
        shouldCreateTask: true,
        shouldUpdateExisting: false,
        title: match[1].trim().slice(0, 200),
        description: null,
        priority: determinePriority(normalizedText),
        assigneeHint: extractAssigneeHint(normalizedText, mentions),
        dueDateHint: extractDueDate(normalizedText),
        confidence: 1.0,
        reasoning: "コマンドによる明示的タスク作成",
        isCompletion: false,
      };
    }
  }

  // 完了キーワードチェック (既存タスクの更新)
  const isCompletion = COMPLETION_KEYWORDS.some((kw) =>
    normalizedText.includes(kw)
  );
  if (isCompletion) {
    return {
      shouldCreateTask: false,
      shouldUpdateExisting: true,
      title: "",
      description: null,
      priority: "medium",
      assigneeHint: authorName ?? null,
      dueDateHint: null,
      confidence: 0.7,
      reasoning: `完了キーワード検出: ${normalizedText.slice(0, 100)}`,
      isCompletion: true,
    };
  }

  // パターンマッチによるタスク検出
  for (const pattern of TASK_PATTERNS) {
    const match = normalizedText.match(pattern);
    if (match) {
      const title = match[1].trim().slice(0, 200);
      if (title.length < 3) continue;

      return {
        shouldCreateTask: true,
        shouldUpdateExisting: false,
        title,
        description: normalizedText.length > 200 ? normalizedText : null,
        priority: determinePriority(normalizedText),
        assigneeHint: extractAssigneeHint(normalizedText, mentions),
        dueDateHint: extractDueDate(normalizedText),
        confidence: 0.6,
        reasoning: `パターンマッチ: ${pattern.source}`,
        isCompletion: false,
      };
    }
  }

  return {
    shouldCreateTask: false,
    shouldUpdateExisting: false,
    title: "",
    description: null,
    priority: "medium",
    assigneeHint: null,
    dueDateHint: null,
    confidence: 0,
    reasoning: "タスク関連のパターンが検出されませんでした",
    isCompletion: false,
  };
}

/**
 * テキストから優先度を判定
 */
function determinePriority(text: string): TaskPriority {
  const hasUrgency = URGENCY_KEYWORDS.some((kw) => text.includes(kw));
  if (hasUrgency) return "critical";

  if (/重要|important|高優先/i.test(text)) return "high";
  if (/できれば|余裕があれば|低優先|low\s*priority/i.test(text)) return "low";

  return "medium";
}

/**
 * テキストからアサインのヒントを抽出
 */
function extractAssigneeHint(
  text: string,
  mentions?: string[]
): string | null {
  // メンションがあればそれを使う
  if (mentions && mentions.length > 0) {
    return mentions[0];
  }

  // @mention パターン
  const mentionMatch = text.match(/@(\w+)/);
  if (mentionMatch) {
    return mentionMatch[1];
  }

  return null;
}

/**
 * テキストから納期を抽出
 */
function extractDueDate(text: string): string | null {
  for (const { pattern, resolver } of DUE_DATE_PATTERNS) {
    const match = text.match(pattern);
    if (match) {
      return resolver(match);
    }
  }
  return null;
}
