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
