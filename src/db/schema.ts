/**
 * Discutere — Database Schema
 *
 * Schedula の MACHINA モジュールを独立サービスとして再構成。
 * workspace_id で外部サービスのグループ/組織を参照する。
 */
import { sqliteTable, text, integer, index, unique } from "drizzle-orm/sqlite-core";

// ── Users (Cernere から admission されたユーザー) ────

export const users = sqliteTable("users", {
  id: text("id").primaryKey(),
  login: text("login").notNull(),
  displayName: text("display_name").notNull(),
  email: text("email"),
  avatarUrl: text("avatar_url").notNull().default(""),
  role: text("role").notNull().default("general"),
  createdAt: integer("created_at", { mode: "timestamp" })
    .$defaultFn(() => new Date())
    .notNull(),
  updatedAt: integer("updated_at", { mode: "timestamp" })
    .$defaultFn(() => new Date())
    .notNull(),
});

// ── Channel Monitors (チャンネル監視設定) ────────────

export const channelMonitors = sqliteTable(
  "channel_monitors",
  {
    id: text("id").primaryKey(),
    workspaceId: text("workspace_id").notNull(),
    platform: text("platform").notNull(), // slack / discord
    channelId: text("channel_id").notNull(),
    channelName: text("channel_name").notNull(),
    webhookEndpointId: text("webhook_endpoint_id"),
    // ── BOT 接続情報 ──
    // Slack: bot user OAuth token (xoxb-...), Discord: bot token
    botToken: text("bot_token"),
    // Slack: workspace id (Txxx), Discord: guild id
    botWorkspaceId: text("bot_workspace_id"),
    // Slack: Signing Secret / Discord: Application Public Key
    botSigningSecret: text("bot_signing_secret"),
    // 取り込みフラグ: BOT 経由でメッセージを取得・保存するか
    captureMessages: integer("capture_messages", { mode: "boolean" }).notNull().default(true),
    // モード: task / discussion / none
    mode: text("mode").notNull().default("task"),
    // discussion モードの遅延処理間隔 (分)
    discussionDelayMinutes: integer("discussion_delay_minutes").notNull().default(5),
    // discussion モードの要約先 (GitHub) 設定
    githubRepo: text("github_repo"),
    githubDiscussionCategoryId: text("github_discussion_category_id"),
    isActive: integer("is_active", { mode: "boolean" }).notNull().default(true),
    createdBy: text("created_by").notNull(),
    createdAt: integer("created_at", { mode: "timestamp" })
      .$defaultFn(() => new Date())
      .notNull(),
    updatedAt: integer("updated_at", { mode: "timestamp" })
      .$defaultFn(() => new Date())
      .notNull(),
  },
  (table) => [
    index("idx_monitor_workspace").on(table.workspaceId),
    unique("unique_monitor_channel").on(table.workspaceId, table.platform, table.channelId),
  ]
);

// ── Chat Messages (取り込んだメッセージログ) ─────────

export const chatMessages = sqliteTable(
  "chat_messages",
  {
    id: text("id").primaryKey(),
    monitorId: text("monitor_id")
      .references(() => channelMonitors.id, { onDelete: "cascade" })
      .notNull(),
    workspaceId: text("workspace_id").notNull(),
    platform: text("platform").notNull(),
    channelId: text("channel_id").notNull(),
    messageId: text("message_id").notNull(),
    authorId: text("author_id").notNull(),
    authorName: text("author_name").notNull(),
    text: text("text").notNull(),
    // メタ情報 (JSON) — mentions, thread_ts, attachments など
    meta: text("meta"),
    // メッセージの発言時刻 (Unix ms)
    postedAt: integer("posted_at", { mode: "timestamp_ms" }).notNull(),
    createdAt: integer("created_at", { mode: "timestamp" })
      .$defaultFn(() => new Date())
      .notNull(),
  },
  (table) => [
    index("idx_chat_monitor").on(table.monitorId),
    index("idx_chat_workspace").on(table.workspaceId),
    index("idx_chat_posted").on(table.postedAt),
    unique("unique_chat_msg").on(table.monitorId, table.messageId),
  ]
);

// ── Chat Summaries (チャンネルの要約) ───────────────

export const chatSummaries = sqliteTable(
  "chat_summaries",
  {
    id: text("id").primaryKey(),
    monitorId: text("monitor_id")
      .references(() => channelMonitors.id, { onDelete: "cascade" })
      .notNull(),
    workspaceId: text("workspace_id").notNull(),
    // 要約の対象期間
    periodStart: integer("period_start", { mode: "timestamp_ms" }).notNull(),
    periodEnd: integer("period_end", { mode: "timestamp_ms" }).notNull(),
    // 要約本文
    summary: text("summary").notNull(),
    // 補足 (JSON): topKeywords, participants, messageCount 等
    highlights: text("highlights"),
    messageCount: integer("message_count").notNull().default(0),
    createdBy: text("created_by").notNull(),
    createdAt: integer("created_at", { mode: "timestamp" })
      .$defaultFn(() => new Date())
      .notNull(),
  },
  (table) => [
    index("idx_summary_monitor").on(table.monitorId),
    index("idx_summary_workspace").on(table.workspaceId),
  ]
);

// ── Tasks (自動生成タスク) ───────────────────────────

export const tasks = sqliteTable(
  "tasks",
  {
    id: text("id").primaryKey(),
    workspaceId: text("workspace_id").notNull(),
    title: text("title").notNull(),
    description: text("description"),
    status: text("status").notNull().default("pending"),
    priority: text("priority").notNull().default("medium"),
    assigneeId: text("assignee_id"),
    dueDate: text("due_date"),
    source: text("source").notNull().default("auto"),
    sourcePlatform: text("source_platform"),
    sourceMessageId: text("source_message_id"),
    sourceChannelId: text("source_channel_id"),
    sourceText: text("source_text"),
    confidence: integer("confidence").notNull().default(0),
    isCriticalPath: integer("is_critical_path", { mode: "boolean" }).notNull().default(false),
    relayedToExternal: integer("relayed_to_external", { mode: "boolean" }).notNull().default(false),
    externalTaskId: text("external_task_id"),
    createdBy: text("created_by").notNull(),
    createdAt: integer("created_at", { mode: "timestamp" })
      .$defaultFn(() => new Date())
      .notNull(),
    updatedAt: integer("updated_at", { mode: "timestamp" })
      .$defaultFn(() => new Date())
      .notNull(),
  },
  (table) => [
    index("idx_task_workspace").on(table.workspaceId),
    index("idx_task_status").on(table.status),
    index("idx_task_assignee").on(table.assigneeId),
    index("idx_task_due").on(table.dueDate),
    index("idx_task_priority").on(table.priority),
  ]
);

// ── Task Activity Log (タスク変更履歴) ──────────────

export const taskLogs = sqliteTable(
  "task_logs",
  {
    id: text("id").primaryKey(),
    taskId: text("task_id")
      .references(() => tasks.id)
      .notNull(),
    action: text("action").notNull(),
    previousValue: text("previous_value"),
    newValue: text("new_value"),
    reason: text("reason"),
    triggerMessageId: text("trigger_message_id"),
    performedBy: text("performed_by").notNull(),
    createdAt: integer("created_at", { mode: "timestamp" })
      .$defaultFn(() => new Date())
      .notNull(),
  },
  (table) => [
    index("idx_log_task").on(table.taskId),
  ]
);
