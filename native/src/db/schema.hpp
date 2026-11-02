#pragma once
// Discutere — サイドカー SQLite スキーマ (src/db/schema.ts の drizzle-orm 定義の置き換え)。
// users / channelMonitors / ludus メカニクス・学習ジョブ / economy-graph 等を持つ。
//
// drizzle のクエリ API は使わない。後続のモジュールは生 SQL で書き、ここの行 struct・テーブル名を使う。
// DDL は drizzle-kit が schema.ts から作る文と 1 文字も違わない (golden: native/tests/fixtures/db/schema.json)。
// 列の型の読み方:
//   integer {mode:"boolean"} → bool (0/1)
//   integer {mode:"timestamp"} → Unix 秒 (drizzle は Date を秒に丸めて保存する)
//   integer {mode:"timestamp_ms"} → Unix ミリ秒
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "base/sqlite.hpp"

namespace di::db {

/** channel_monitors (schema.ts の channelMonitors)。 */
struct ChannelMonitor {
    static constexpr const char* TABLE = "channel_monitors";
    std::string id;
    std::string workspaceId;
    std::string platform;
    std::string channelId;
    std::string channelName;
    std::optional<std::string> webhookEndpointId;
    std::optional<std::string> botToken;
    std::optional<std::string> botWorkspaceId;
    std::optional<std::string> botSigningSecret;
    bool captureMessages = false; // integer {mode: "boolean"} (0/1)
    std::string mode;
    std::int64_t discussionDelayMinutes = 0;
    std::optional<std::string> githubRepo;
    std::optional<std::string> githubDiscussionCategoryId;
    bool isActive = false; // integer {mode: "boolean"} (0/1)
    std::string createdBy;
    std::int64_t createdAt = 0; // integer {mode: "timestamp"} (Unix 秒。drizzle は new Date() を秒で保存する)
    std::int64_t updatedAt = 0; // integer {mode: "timestamp"} (Unix 秒。drizzle は new Date() を秒で保存する)
    static ChannelMonitor from_row(const sql::Row& row);
};

/** chat_messages (schema.ts の chatMessages)。 */
struct ChatMessage {
    static constexpr const char* TABLE = "chat_messages";
    std::string id;
    std::string monitorId;
    std::string workspaceId;
    std::string platform;
    std::string channelId;
    std::string messageId;
    std::string authorId;
    std::string authorName;
    std::string text;
    std::optional<std::string> meta;
    std::int64_t postedAt = 0; // integer {mode: "timestamp_ms"} (Unix ミリ秒)
    std::int64_t createdAt = 0; // integer {mode: "timestamp"} (Unix 秒。drizzle は new Date() を秒で保存する)
    static ChatMessage from_row(const sql::Row& row);
};

/** chat_summaries (schema.ts の chatSummaries)。 */
struct ChatSummary {
    static constexpr const char* TABLE = "chat_summaries";
    std::string id;
    std::string monitorId;
    std::string workspaceId;
    std::int64_t periodStart = 0; // integer {mode: "timestamp_ms"} (Unix ミリ秒)
    std::int64_t periodEnd = 0; // integer {mode: "timestamp_ms"} (Unix ミリ秒)
    std::string summary;
    std::optional<std::string> highlights;
    std::int64_t messageCount = 0;
    std::string createdBy;
    std::int64_t createdAt = 0; // integer {mode: "timestamp"} (Unix 秒。drizzle は new Date() を秒で保存する)
    static ChatSummary from_row(const sql::Row& row);
};

/** ludus_economy_graphs (schema.ts の ludusEconomyGraphs)。 */
struct LudusEconomyGraph {
    static constexpr const char* TABLE = "ludus_economy_graphs";
    std::string id;
    std::string workspaceId;
    std::string gameTitle;
    std::string graphJson;
    std::int64_t mechanicCount = 0;
    std::int64_t createdAt = 0; // integer {mode: "timestamp"} (Unix 秒。drizzle は new Date() を秒で保存する)
    std::int64_t updatedAt = 0; // integer {mode: "timestamp"} (Unix 秒。drizzle は new Date() を秒で保存する)
    static LudusEconomyGraph from_row(const sql::Row& row);
};

/** ludus_learning_jobs (schema.ts の ludusLearningJobs)。 */
struct LudusLearningJob {
    static constexpr const char* TABLE = "ludus_learning_jobs";
    std::string id;
    std::string workspaceId;
    std::string gameTitle;
    std::string query;
    std::string status;
    std::int64_t fetchedCount = 0;
    std::int64_t learnedCount = 0;
    std::optional<std::string> error;
    std::int64_t createdAt = 0; // integer {mode: "timestamp"} (Unix 秒。drizzle は new Date() を秒で保存する)
    std::int64_t updatedAt = 0; // integer {mode: "timestamp"} (Unix 秒。drizzle は new Date() を秒で保存する)
    static LudusLearningJob from_row(const sql::Row& row);
};

/** ludus_mechanics (schema.ts の ludusMechanics)。 */
struct LudusMechanic {
    static constexpr const char* TABLE = "ludus_mechanics";
    std::string id;
    std::string workspaceId;
    std::string gameTitle;
    std::string mechanic;
    std::int64_t confidence = 0;
    std::string sourceUrl;
    std::optional<std::string> sourceTitle;
    std::optional<std::string> sourceSnippet;
    std::optional<std::string> tagsJson;
    std::int64_t createdAt = 0; // integer {mode: "timestamp"} (Unix 秒。drizzle は new Date() を秒で保存する)
    static LudusMechanic from_row(const sql::Row& row);
};

/** task_logs (schema.ts の taskLogs)。 */
struct TaskLog {
    static constexpr const char* TABLE = "task_logs";
    std::string id;
    std::string taskId;
    std::string action;
    std::optional<std::string> previousValue;
    std::optional<std::string> newValue;
    std::optional<std::string> reason;
    std::optional<std::string> triggerMessageId;
    std::string performedBy;
    std::int64_t createdAt = 0; // integer {mode: "timestamp"} (Unix 秒。drizzle は new Date() を秒で保存する)
    static TaskLog from_row(const sql::Row& row);
};

/** tasks (schema.ts の tasks)。 */
struct Task {
    static constexpr const char* TABLE = "tasks";
    std::string id;
    std::string workspaceId;
    std::string title;
    std::optional<std::string> description;
    std::string status;
    std::string priority;
    std::optional<std::string> assigneeId;
    std::optional<std::string> dueDate;
    std::string source;
    std::optional<std::string> sourcePlatform;
    std::optional<std::string> sourceMessageId;
    std::optional<std::string> sourceChannelId;
    std::optional<std::string> sourceText;
    std::int64_t confidence = 0;
    bool isCriticalPath = false; // integer {mode: "boolean"} (0/1)
    bool relayedToExternal = false; // integer {mode: "boolean"} (0/1)
    std::optional<std::string> externalTaskId;
    std::string createdBy;
    std::int64_t createdAt = 0; // integer {mode: "timestamp"} (Unix 秒。drizzle は new Date() を秒で保存する)
    std::int64_t updatedAt = 0; // integer {mode: "timestamp"} (Unix 秒。drizzle は new Date() を秒で保存する)
    static Task from_row(const sql::Row& row);
};

/** users (schema.ts の users)。 */
struct User {
    static constexpr const char* TABLE = "users";
    std::string id;
    std::string login;
    std::string displayName;
    std::optional<std::string> email;
    std::string avatarUrl;
    std::string role;
    std::int64_t createdAt = 0; // integer {mode: "timestamp"} (Unix 秒。drizzle は new Date() を秒で保存する)
    std::int64_t updatedAt = 0; // integer {mode: "timestamp"} (Unix 秒。drizzle は new Date() を秒で保存する)
    static User from_row(const sql::Row& row);
};

/** 1 テーブルぶんの DDL (CREATE TABLE と、それに続く CREATE INDEX)。 */
struct TableDdl {
    std::string table;
    std::vector<std::string> statements;
};

/** drizzle-kit generate が出す順の DDL。 */
const std::vector<TableDdl>& schema_ddl();

/**
 * 無いテーブルだけを作る (drizzle-kit push の代わり。既存のテーブル・列には触らない)。
 * 作ったテーブル名を返す。
 */
std::vector<std::string> ensure_schema(sql::Database& db);

} // namespace di::db
