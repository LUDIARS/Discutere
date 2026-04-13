// Task statuses
export const TASK_STATUSES = ["pending", "in_progress", "done", "cancelled"] as const;
export type TaskStatus = (typeof TASK_STATUSES)[number];

// Priorities
export const TASK_PRIORITIES = ["low", "medium", "high", "critical"] as const;
export type TaskPriority = (typeof TASK_PRIORITIES)[number];

// Sources
export const TASK_SOURCES = ["auto", "command", "manual"] as const;

// Platforms
export const MONITOR_PLATFORMS = ["slack", "discord"] as const;

// Channel modes — 各チャンネル(ツリー)が「なにをするか」
//   task       : 投稿を即時処理、Haiku でタスク性判定、不足情報はヒアリング
//   discussion : 遅延処理でツリーをまとめて要約し、GitHub Discussion に保存
//   none       : 何もしない (ログ保存のみ)
export const CHANNEL_MODES = ["task", "discussion", "none"] as const;
export type ChannelMode = (typeof CHANNEL_MODES)[number];

// Completion keywords
export const COMPLETION_KEYWORDS = [
  "完了", "done", "修正した", "修正済み", "対応した", "対応済み",
  "解決", "resolved", "fixed", "closed", "終わった", "終了",
];

// Urgency keywords
export const URGENCY_KEYWORDS = [
  "急ぎ", "至急", "ASAP", "asap", "urgent", "緊急", "今すぐ", "早急",
];

// Log actions
export const LOG_ACTIONS = [
  "created", "updated", "assigned", "status_changed", "priority_changed", "relayed",
] as const;
