#include "db/schema.hpp"

namespace di::db {

ChannelMonitor ChannelMonitor::from_row(const sql::Row& row) {
    ChannelMonitor r;
    r.id = row.str("id");
    r.workspaceId = row.str("workspace_id");
    r.platform = row.str("platform");
    r.channelId = row.str("channel_id");
    r.channelName = row.str("channel_name");
    r.webhookEndpointId = row.opt_str("webhook_endpoint_id");
    r.botToken = row.opt_str("bot_token");
    r.botWorkspaceId = row.opt_str("bot_workspace_id");
    r.botSigningSecret = row.opt_str("bot_signing_secret");
    r.captureMessages = row.i64("capture_messages") != 0;
    r.mode = row.str("mode");
    r.discussionDelayMinutes = row.i64("discussion_delay_minutes");
    r.githubRepo = row.opt_str("github_repo");
    r.githubDiscussionCategoryId = row.opt_str("github_discussion_category_id");
    r.isActive = row.i64("is_active") != 0;
    r.createdBy = row.str("created_by");
    r.createdAt = row.i64("created_at");
    r.updatedAt = row.i64("updated_at");
    return r;
}

ChatMessage ChatMessage::from_row(const sql::Row& row) {
    ChatMessage r;
    r.id = row.str("id");
    r.monitorId = row.str("monitor_id");
    r.workspaceId = row.str("workspace_id");
    r.platform = row.str("platform");
    r.channelId = row.str("channel_id");
    r.messageId = row.str("message_id");
    r.authorId = row.str("author_id");
    r.authorName = row.str("author_name");
    r.text = row.str("text");
    r.meta = row.opt_str("meta");
    r.postedAt = row.i64("posted_at");
    r.createdAt = row.i64("created_at");
    return r;
}

ChatSummary ChatSummary::from_row(const sql::Row& row) {
    ChatSummary r;
    r.id = row.str("id");
    r.monitorId = row.str("monitor_id");
    r.workspaceId = row.str("workspace_id");
    r.periodStart = row.i64("period_start");
    r.periodEnd = row.i64("period_end");
    r.summary = row.str("summary");
    r.highlights = row.opt_str("highlights");
    r.messageCount = row.i64("message_count");
    r.createdBy = row.str("created_by");
    r.createdAt = row.i64("created_at");
    return r;
}

LudusEconomyGraph LudusEconomyGraph::from_row(const sql::Row& row) {
    LudusEconomyGraph r;
    r.id = row.str("id");
    r.workspaceId = row.str("workspace_id");
    r.gameTitle = row.str("game_title");
    r.graphJson = row.str("graph_json");
    r.mechanicCount = row.i64("mechanic_count");
    r.createdAt = row.i64("created_at");
    r.updatedAt = row.i64("updated_at");
    return r;
}

LudusLearningJob LudusLearningJob::from_row(const sql::Row& row) {
    LudusLearningJob r;
    r.id = row.str("id");
    r.workspaceId = row.str("workspace_id");
    r.gameTitle = row.str("game_title");
    r.query = row.str("query");
    r.status = row.str("status");
    r.fetchedCount = row.i64("fetched_count");
    r.learnedCount = row.i64("learned_count");
    r.error = row.opt_str("error");
    r.createdAt = row.i64("created_at");
    r.updatedAt = row.i64("updated_at");
    return r;
}

LudusMechanic LudusMechanic::from_row(const sql::Row& row) {
    LudusMechanic r;
    r.id = row.str("id");
    r.workspaceId = row.str("workspace_id");
    r.gameTitle = row.str("game_title");
    r.mechanic = row.str("mechanic");
    r.confidence = row.i64("confidence");
    r.sourceUrl = row.str("source_url");
    r.sourceTitle = row.opt_str("source_title");
    r.sourceSnippet = row.opt_str("source_snippet");
    r.tagsJson = row.opt_str("tags_json");
    r.createdAt = row.i64("created_at");
    return r;
}

TaskLog TaskLog::from_row(const sql::Row& row) {
    TaskLog r;
    r.id = row.str("id");
    r.taskId = row.str("task_id");
    r.action = row.str("action");
    r.previousValue = row.opt_str("previous_value");
    r.newValue = row.opt_str("new_value");
    r.reason = row.opt_str("reason");
    r.triggerMessageId = row.opt_str("trigger_message_id");
    r.performedBy = row.str("performed_by");
    r.createdAt = row.i64("created_at");
    return r;
}

Task Task::from_row(const sql::Row& row) {
    Task r;
    r.id = row.str("id");
    r.workspaceId = row.str("workspace_id");
    r.title = row.str("title");
    r.description = row.opt_str("description");
    r.status = row.str("status");
    r.priority = row.str("priority");
    r.assigneeId = row.opt_str("assignee_id");
    r.dueDate = row.opt_str("due_date");
    r.source = row.str("source");
    r.sourcePlatform = row.opt_str("source_platform");
    r.sourceMessageId = row.opt_str("source_message_id");
    r.sourceChannelId = row.opt_str("source_channel_id");
    r.sourceText = row.opt_str("source_text");
    r.confidence = row.i64("confidence");
    r.isCriticalPath = row.i64("is_critical_path") != 0;
    r.relayedToExternal = row.i64("relayed_to_external") != 0;
    r.externalTaskId = row.opt_str("external_task_id");
    r.createdBy = row.str("created_by");
    r.createdAt = row.i64("created_at");
    r.updatedAt = row.i64("updated_at");
    return r;
}

User User::from_row(const sql::Row& row) {
    User r;
    r.id = row.str("id");
    r.login = row.str("login");
    r.displayName = row.str("display_name");
    r.email = row.opt_str("email");
    r.avatarUrl = row.str("avatar_url");
    r.role = row.str("role");
    r.createdAt = row.i64("created_at");
    r.updatedAt = row.i64("updated_at");
    return r;
}
const std::vector<TableDdl>& schema_ddl() {
    static const std::vector<TableDdl> ddl = {
        {"channel_monitors",
         {
          R"SQL(CREATE TABLE `channel_monitors` (
	`id` text PRIMARY KEY NOT NULL,
	`workspace_id` text NOT NULL,
	`platform` text NOT NULL,
	`channel_id` text NOT NULL,
	`channel_name` text NOT NULL,
	`webhook_endpoint_id` text,
	`bot_token` text,
	`bot_workspace_id` text,
	`bot_signing_secret` text,
	`capture_messages` integer DEFAULT true NOT NULL,
	`mode` text DEFAULT 'task' NOT NULL,
	`discussion_delay_minutes` integer DEFAULT 5 NOT NULL,
	`github_repo` text,
	`github_discussion_category_id` text,
	`is_active` integer DEFAULT true NOT NULL,
	`created_by` text NOT NULL,
	`created_at` integer NOT NULL,
	`updated_at` integer NOT NULL
);
)SQL",
          R"SQL(CREATE INDEX `idx_monitor_workspace` ON `channel_monitors` (`workspace_id`);)SQL",
          R"SQL(CREATE UNIQUE INDEX `unique_monitor_channel` ON `channel_monitors` (`workspace_id`,`platform`,`channel_id`);)SQL"}},
        {"chat_messages",
         {
          R"SQL(CREATE TABLE `chat_messages` (
	`id` text PRIMARY KEY NOT NULL,
	`monitor_id` text NOT NULL,
	`workspace_id` text NOT NULL,
	`platform` text NOT NULL,
	`channel_id` text NOT NULL,
	`message_id` text NOT NULL,
	`author_id` text NOT NULL,
	`author_name` text NOT NULL,
	`text` text NOT NULL,
	`meta` text,
	`posted_at` integer NOT NULL,
	`created_at` integer NOT NULL,
	FOREIGN KEY (`monitor_id`) REFERENCES `channel_monitors`(`id`) ON UPDATE no action ON DELETE cascade
);
)SQL",
          R"SQL(CREATE INDEX `idx_chat_monitor` ON `chat_messages` (`monitor_id`);)SQL",
          R"SQL(CREATE INDEX `idx_chat_workspace` ON `chat_messages` (`workspace_id`);)SQL",
          R"SQL(CREATE INDEX `idx_chat_posted` ON `chat_messages` (`posted_at`);)SQL",
          R"SQL(CREATE UNIQUE INDEX `unique_chat_msg` ON `chat_messages` (`monitor_id`,`message_id`);)SQL"}},
        {"chat_summaries",
         {
          R"SQL(CREATE TABLE `chat_summaries` (
	`id` text PRIMARY KEY NOT NULL,
	`monitor_id` text NOT NULL,
	`workspace_id` text NOT NULL,
	`period_start` integer NOT NULL,
	`period_end` integer NOT NULL,
	`summary` text NOT NULL,
	`highlights` text,
	`message_count` integer DEFAULT 0 NOT NULL,
	`created_by` text NOT NULL,
	`created_at` integer NOT NULL,
	FOREIGN KEY (`monitor_id`) REFERENCES `channel_monitors`(`id`) ON UPDATE no action ON DELETE cascade
);
)SQL",
          R"SQL(CREATE INDEX `idx_summary_monitor` ON `chat_summaries` (`monitor_id`);)SQL",
          R"SQL(CREATE INDEX `idx_summary_workspace` ON `chat_summaries` (`workspace_id`);)SQL"}},
        {"ludus_economy_graphs",
         {
          R"SQL(CREATE TABLE `ludus_economy_graphs` (
	`id` text PRIMARY KEY NOT NULL,
	`workspace_id` text NOT NULL,
	`game_title` text NOT NULL,
	`graph_json` text NOT NULL,
	`mechanic_count` integer DEFAULT 0 NOT NULL,
	`created_at` integer NOT NULL,
	`updated_at` integer NOT NULL
);
)SQL",
          R"SQL(CREATE INDEX `idx_eco_graph_workspace` ON `ludus_economy_graphs` (`workspace_id`);)SQL",
          R"SQL(CREATE INDEX `idx_eco_graph_game` ON `ludus_economy_graphs` (`game_title`);)SQL",
          R"SQL(CREATE UNIQUE INDEX `unique_eco_graph_game` ON `ludus_economy_graphs` (`workspace_id`,`game_title`);)SQL"}},
        {"ludus_learning_jobs",
         {
          R"SQL(CREATE TABLE `ludus_learning_jobs` (
	`id` text PRIMARY KEY NOT NULL,
	`workspace_id` text NOT NULL,
	`game_title` text NOT NULL,
	`query` text NOT NULL,
	`status` text DEFAULT 'queued' NOT NULL,
	`fetched_count` integer DEFAULT 0 NOT NULL,
	`learned_count` integer DEFAULT 0 NOT NULL,
	`error` text,
	`created_at` integer NOT NULL,
	`updated_at` integer NOT NULL
);
)SQL",
          R"SQL(CREATE INDEX `idx_ludus_jobs_workspace` ON `ludus_learning_jobs` (`workspace_id`);)SQL",
          R"SQL(CREATE INDEX `idx_ludus_jobs_status` ON `ludus_learning_jobs` (`status`);)SQL"}},
        {"ludus_mechanics",
         {
          R"SQL(CREATE TABLE `ludus_mechanics` (
	`id` text PRIMARY KEY NOT NULL,
	`workspace_id` text NOT NULL,
	`game_title` text NOT NULL,
	`mechanic` text NOT NULL,
	`confidence` integer DEFAULT 0 NOT NULL,
	`source_url` text NOT NULL,
	`source_title` text,
	`source_snippet` text,
	`tags_json` text,
	`created_at` integer NOT NULL
);
)SQL",
          R"SQL(CREATE INDEX `idx_ludus_workspace` ON `ludus_mechanics` (`workspace_id`);)SQL",
          R"SQL(CREATE INDEX `idx_ludus_game` ON `ludus_mechanics` (`game_title`);)SQL",
          R"SQL(CREATE INDEX `idx_ludus_mechanic` ON `ludus_mechanics` (`mechanic`);)SQL",
          R"SQL(CREATE UNIQUE INDEX `unique_ludus_source_mechanic` ON `ludus_mechanics` (`workspace_id`,`source_url`,`mechanic`);)SQL"}},
        {"task_logs",
         {
          R"SQL(CREATE TABLE `task_logs` (
	`id` text PRIMARY KEY NOT NULL,
	`task_id` text NOT NULL,
	`action` text NOT NULL,
	`previous_value` text,
	`new_value` text,
	`reason` text,
	`trigger_message_id` text,
	`performed_by` text NOT NULL,
	`created_at` integer NOT NULL,
	FOREIGN KEY (`task_id`) REFERENCES `tasks`(`id`) ON UPDATE no action ON DELETE no action
);
)SQL",
          R"SQL(CREATE INDEX `idx_log_task` ON `task_logs` (`task_id`);)SQL"}},
        {"tasks",
         {
          R"SQL(CREATE TABLE `tasks` (
	`id` text PRIMARY KEY NOT NULL,
	`workspace_id` text NOT NULL,
	`title` text NOT NULL,
	`description` text,
	`status` text DEFAULT 'pending' NOT NULL,
	`priority` text DEFAULT 'medium' NOT NULL,
	`assignee_id` text,
	`due_date` text,
	`source` text DEFAULT 'auto' NOT NULL,
	`source_platform` text,
	`source_message_id` text,
	`source_channel_id` text,
	`source_text` text,
	`confidence` integer DEFAULT 0 NOT NULL,
	`is_critical_path` integer DEFAULT false NOT NULL,
	`relayed_to_external` integer DEFAULT false NOT NULL,
	`external_task_id` text,
	`created_by` text NOT NULL,
	`created_at` integer NOT NULL,
	`updated_at` integer NOT NULL
);
)SQL",
          R"SQL(CREATE INDEX `idx_task_workspace` ON `tasks` (`workspace_id`);)SQL",
          R"SQL(CREATE INDEX `idx_task_status` ON `tasks` (`status`);)SQL",
          R"SQL(CREATE INDEX `idx_task_assignee` ON `tasks` (`assignee_id`);)SQL",
          R"SQL(CREATE INDEX `idx_task_due` ON `tasks` (`due_date`);)SQL",
          R"SQL(CREATE INDEX `idx_task_priority` ON `tasks` (`priority`);)SQL"}},
        {"users",
         {
          R"SQL(CREATE TABLE `users` (
	`id` text PRIMARY KEY NOT NULL,
	`login` text NOT NULL,
	`display_name` text NOT NULL,
	`email` text,
	`avatar_url` text DEFAULT '' NOT NULL,
	`role` text DEFAULT 'general' NOT NULL,
	`created_at` integer NOT NULL,
	`updated_at` integer NOT NULL
);
)SQL"}},
    };
    return ddl;
}

std::vector<std::string> ensure_schema(sql::Database& db) {
    std::vector<std::string> created;
    std::lock_guard lock(db.mutex());
    db.transaction([&] {
        for (const auto& t : schema_ddl()) {
            if (db.table_exists(t.table)) continue;
            for (const auto& stmt : t.statements) db.exec(stmt);
            created.push_back(t.table);
        }
    });
    return created;
}

} // namespace di::db
