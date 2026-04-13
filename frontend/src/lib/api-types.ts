// ── Machina Task Types ────────────────────────────────────

export interface MachinaTaskItem {
  id: string;
  workspaceId: string;
  title: string;
  description: string | null;
  status: string;
  priority: string;
  assigneeId: string | null;
  assigneeName?: string | null;
  dueDate: string | null;
  source: string;
  sourcePlatform: string | null;
  sourceMessageId: string | null;
  sourceChannelId: string | null;
  sourceText: string | null;
  confidence: number;
  isCriticalPath: boolean;
  relayedToExternal: boolean;
  externalTaskId: string | null;
  /** UI 互換: PM リレー済みフラグ */
  relayedToPm?: boolean;
  /** UI 互換: PM タスク ID */
  pmTaskId?: string | null;
  createdBy: string;
  createdAt: string;
  updatedAt: string;
}

export interface MachinaTaskLogItem {
  id: string;
  taskId: string;
  action: string;
  previousValue: string | null;
  newValue: string | null;
  reason: string | null;
  triggerMessageId: string | null;
  performedBy: string;
  createdAt: string;
}

// ── Channel Monitor / Bot Types ───────────────────────────

export type ChannelMode = "task" | "discussion" | "none";

export interface MachinaChannelMonitorItem {
  id: string;
  workspaceId: string;
  platform: string;
  channelId: string;
  channelName: string;
  webhookEndpointId: string | null;
  botWorkspaceId: string | null;
  hasBotToken: boolean;
  hasBotSigningSecret: boolean;
  captureMessages: boolean;
  isActive: boolean;
  mode: ChannelMode;
  discussionDelayMinutes: number;
  githubRepo: string | null;
  githubDiscussionCategoryId: string | null;
  pendingTaskSessions: number;
  pendingDiscussionSessions: number;
  createdBy: string;
  createdAt: string;
  updatedAt: string;
}

// ── Mode Session Types ────────────────────────────────────

export interface ModeTaskSessionItem {
  id: string;
  monitorId: string;
  workspaceId: string;
  platform: "slack" | "discord";
  channelId: string;
  threadKey: string;
  status: "classifying" | "hearing" | "collecting" | "registering" | "failed";
  messages: Array<{
    authorId: string;
    authorName: string;
    text: string;
    messageId: string;
    postedAt: string;
  }>;
  classification: {
    isTask: boolean;
    confidence: number;
    missingFields: string[];
    title?: string;
    description?: string;
    priority?: string;
    reasoning: string;
  } | null;
  errorReason: string | null;
  taskId: string | null;
  isStalled: boolean;
  createdAt: string;
  updatedAt: string;
}

export interface ModeDiscussionSessionItem {
  id: string;
  monitorId: string;
  workspaceId: string;
  scheduledAt: string;
  windowStart: string;
  status: "pending" | "summarizing" | "publishing" | "failed";
  errorReason: string | null;
  lastPublishedUrl: string | null;
  isStalled: boolean;
  createdAt: string;
  updatedAt: string;
}

// ── Chat Log / Summary Types ──────────────────────────────

export interface MachinaChatMessageItem {
  id: string;
  monitorId: string;
  workspaceId: string;
  platform: string;
  channelId: string;
  messageId: string;
  authorId: string;
  authorName: string;
  text: string;
  meta: string | null;
  postedAt: string;
  createdAt: string;
}

export interface MachinaSummaryHighlights {
  participants: Array<{ authorId: string; authorName: string; messageCount: number }>;
  topKeywords: Array<{ keyword: string; count: number }>;
  topMessages: Array<{ authorName: string; text: string; postedAt: string }>;
}

export interface MachinaChatSummaryItem {
  id: string;
  monitorId: string;
  workspaceId: string;
  periodStart: string;
  periodEnd: string;
  summary: string;
  /** JSON シリアライズされた MachinaSummaryHighlights */
  highlights: string | null;
  messageCount: number;
  createdBy: string;
  createdAt: string;
}

// ── Group Types ───────────────────────────────────────────

export interface GroupItem {
  id: string;
  name: string;
}
