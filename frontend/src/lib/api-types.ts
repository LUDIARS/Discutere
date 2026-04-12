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
  createdBy: string;
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
