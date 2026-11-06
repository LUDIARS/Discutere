#include "db/repository.hpp"

#include <cmath>
#include <stdexcept>

#include "base/time.hpp"
#include "db/connection.hpp"

namespace di::db {

namespace {

enum class Kind { text, integer, boolean, timestamp, timestamp_ms };

struct Column {
    const char* prop;
    const char* column;
    Kind kind;
    /** .default(x) (null = 既定なし)。 */
    Json static_default = nullptr;
    /** $defaultFn(() => new Date())。 */
    bool default_now = false;
};

struct TableSpec {
    const char* table;
    std::vector<Column> columns;
};

const TableSpec& users_spec() {
    static const TableSpec s{"users",
                             {{"id", "id", Kind::text},
                              {"login", "login", Kind::text},
                              {"displayName", "display_name", Kind::text},
                              {"email", "email", Kind::text},
                              {"avatarUrl", "avatar_url", Kind::text, ""},
                              {"role", "role", Kind::text, "general"},
                              {"createdAt", "created_at", Kind::timestamp, nullptr, true},
                              {"updatedAt", "updated_at", Kind::timestamp, nullptr, true}}};
    return s;
}

const TableSpec& monitors_spec() {
    static const TableSpec s{"channel_monitors",
                             {{"id", "id", Kind::text},
                              {"workspaceId", "workspace_id", Kind::text},
                              {"platform", "platform", Kind::text},
                              {"channelId", "channel_id", Kind::text},
                              {"channelName", "channel_name", Kind::text},
                              {"webhookEndpointId", "webhook_endpoint_id", Kind::text},
                              {"botToken", "bot_token", Kind::text},
                              {"botWorkspaceId", "bot_workspace_id", Kind::text},
                              {"botSigningSecret", "bot_signing_secret", Kind::text},
                              {"captureMessages", "capture_messages", Kind::boolean, true},
                              {"mode", "mode", Kind::text, "task"},
                              {"discussionDelayMinutes", "discussion_delay_minutes", Kind::integer, 5},
                              {"githubRepo", "github_repo", Kind::text},
                              {"githubDiscussionCategoryId", "github_discussion_category_id", Kind::text},
                              {"isActive", "is_active", Kind::boolean, true},
                              {"createdBy", "created_by", Kind::text},
                              {"createdAt", "created_at", Kind::timestamp, nullptr, true},
                              {"updatedAt", "updated_at", Kind::timestamp, nullptr, true}}};
    return s;
}

const TableSpec& chat_messages_spec() {
    static const TableSpec s{"chat_messages",
                             {{"id", "id", Kind::text},
                              {"monitorId", "monitor_id", Kind::text},
                              {"workspaceId", "workspace_id", Kind::text},
                              {"platform", "platform", Kind::text},
                              {"channelId", "channel_id", Kind::text},
                              {"messageId", "message_id", Kind::text},
                              {"authorId", "author_id", Kind::text},
                              {"authorName", "author_name", Kind::text},
                              {"text", "text", Kind::text},
                              {"meta", "meta", Kind::text},
                              {"postedAt", "posted_at", Kind::timestamp_ms},
                              {"createdAt", "created_at", Kind::timestamp, nullptr, true}}};
    return s;
}

const TableSpec& chat_summaries_spec() {
    static const TableSpec s{"chat_summaries",
                             {{"id", "id", Kind::text},
                              {"monitorId", "monitor_id", Kind::text},
                              {"workspaceId", "workspace_id", Kind::text},
                              {"periodStart", "period_start", Kind::timestamp_ms},
                              {"periodEnd", "period_end", Kind::timestamp_ms},
                              {"summary", "summary", Kind::text},
                              {"highlights", "highlights", Kind::text},
                              {"messageCount", "message_count", Kind::integer, 0},
                              {"createdBy", "created_by", Kind::text},
                              {"createdAt", "created_at", Kind::timestamp, nullptr, true}}};
    return s;
}

const TableSpec& tasks_spec() {
    static const TableSpec s{"tasks",
                             {{"id", "id", Kind::text},
                              {"workspaceId", "workspace_id", Kind::text},
                              {"title", "title", Kind::text},
                              {"description", "description", Kind::text},
                              {"status", "status", Kind::text, "pending"},
                              {"priority", "priority", Kind::text, "medium"},
                              {"assigneeId", "assignee_id", Kind::text},
                              {"dueDate", "due_date", Kind::text},
                              {"source", "source", Kind::text, "auto"},
                              {"sourcePlatform", "source_platform", Kind::text},
                              {"sourceMessageId", "source_message_id", Kind::text},
                              {"sourceChannelId", "source_channel_id", Kind::text},
                              {"sourceText", "source_text", Kind::text},
                              {"confidence", "confidence", Kind::integer, 0},
                              {"isCriticalPath", "is_critical_path", Kind::boolean, false},
                              {"relayedToExternal", "relayed_to_external", Kind::boolean, false},
                              {"externalTaskId", "external_task_id", Kind::text},
                              {"createdBy", "created_by", Kind::text},
                              {"createdAt", "created_at", Kind::timestamp, nullptr, true},
                              {"updatedAt", "updated_at", Kind::timestamp, nullptr, true}}};
    return s;
}

const TableSpec& task_logs_spec() {
    static const TableSpec s{"task_logs",
                             {{"id", "id", Kind::text},
                              {"taskId", "task_id", Kind::text},
                              {"action", "action", Kind::text},
                              {"previousValue", "previous_value", Kind::text},
                              {"newValue", "new_value", Kind::text},
                              {"reason", "reason", Kind::text},
                              {"triggerMessageId", "trigger_message_id", Kind::text},
                              {"performedBy", "performed_by", Kind::text},
                              {"createdAt", "created_at", Kind::timestamp, nullptr, true}}};
    return s;
}

const TableSpec& ludus_mechanics_spec() {
    static const TableSpec s{"ludus_mechanics",
                             {{"id", "id", Kind::text},
                              {"workspaceId", "workspace_id", Kind::text},
                              {"gameTitle", "game_title", Kind::text},
                              {"mechanic", "mechanic", Kind::text},
                              {"confidence", "confidence", Kind::integer, 0},
                              {"sourceUrl", "source_url", Kind::text},
                              {"sourceTitle", "source_title", Kind::text},
                              {"sourceSnippet", "source_snippet", Kind::text},
                              {"tagsJson", "tags_json", Kind::text},
                              {"createdAt", "created_at", Kind::timestamp, nullptr, true}}};
    return s;
}

const TableSpec& learning_jobs_spec() {
    static const TableSpec s{"ludus_learning_jobs",
                             {{"id", "id", Kind::text},
                              {"workspaceId", "workspace_id", Kind::text},
                              {"gameTitle", "game_title", Kind::text},
                              {"query", "query", Kind::text},
                              {"status", "status", Kind::text, "queued"},
                              {"fetchedCount", "fetched_count", Kind::integer, 0},
                              {"learnedCount", "learned_count", Kind::integer, 0},
                              {"error", "error", Kind::text},
                              {"createdAt", "created_at", Kind::timestamp, nullptr, true},
                              {"updatedAt", "updated_at", Kind::timestamp, nullptr, true}}};
    return s;
}

const TableSpec& economy_graphs_spec() {
    static const TableSpec s{"ludus_economy_graphs",
                             {{"id", "id", Kind::text},
                              {"workspaceId", "workspace_id", Kind::text},
                              {"gameTitle", "game_title", Kind::text},
                              {"graphJson", "graph_json", Kind::text},
                              {"mechanicCount", "mechanic_count", Kind::integer, 0},
                              {"createdAt", "created_at", Kind::timestamp, nullptr, true},
                              {"updatedAt", "updated_at", Kind::timestamp, nullptr, true}}};
    return s;
}

std::int64_t now_ms() { return time::now_ms(); }

/** drizzle の mapToDriverValue。 */
sql::Value to_driver(const Column& c, const Json& v) {
    if (v.is_null()) return sql::Value();
    switch (c.kind) {
    case Kind::text:
        if (v.is_string()) return sql::Value(v.get<std::string>());
        if (v.is_number()) return sql::Value(v.get<double>());
        break;
    case Kind::integer:
        if (v.is_number()) return sql::Value(v.get<double>());
        if (v.is_string()) return sql::Value(v.get<std::string>());
        break;
    case Kind::boolean:
        if (v.is_boolean()) return sql::Value(v.get<bool>() ? 1 : 0);
        break;
    case Kind::timestamp:
        // Date → Math.floor(getTime() / 1000)
        if (v.is_number()) {
            const double ms = v.get<double>();
            if (!std::isfinite(ms)) break;
            return sql::Value(static_cast<std::int64_t>(std::floor(ms / 1000.0)));
        }
        break;
    case Kind::timestamp_ms:
        if (v.is_number()) {
            const double ms = v.get<double>();
            if (!std::isfinite(ms)) break;
            return sql::Value(static_cast<std::int64_t>(std::floor(ms)));
        }
        break;
    }
    throw std::invalid_argument(std::string(c.prop) + ": unsupported value type");
}

void insert(sql::Database& db, const TableSpec& spec, const Json& data, bool on_conflict_do_nothing) {
    if (!data.is_object()) throw std::invalid_argument("insert values must be an object");
    std::string cols;
    std::string marks;
    sql::Params params;
    const std::int64_t at = now_ms();
    for (const auto& c : spec.columns) {
        sql::Value v;
        auto it = data.find(c.prop);
        if (it != data.end()) {
            v = to_driver(c, *it);
        } else if (!c.static_default.is_null()) {
            v = to_driver(c, c.static_default);
        } else if (c.default_now) {
            v = to_driver(c, Json(static_cast<double>(at)));
        }
        if (!cols.empty()) {
            cols += ", ";
            marks += ", ";
        }
        cols += "\"";
        cols += c.column;
        cols += "\"";
        marks += "?";
        params.add(std::move(v));
    }
    std::string sql = std::string("INSERT INTO \"") + spec.table + "\" (" + cols + ") VALUES (" + marks + ")";
    if (on_conflict_do_nothing) sql += " ON CONFLICT DO NOTHING";
    db.run(sql, params);
}

/** `.set({ ...data, updatedAt: new Date() }).where(id = ?)`。表に無いキーは無視する。 */
void update_by(sql::Database& db, const TableSpec& spec, const Json& data, const char* where_column, const std::string& where_value) {
    if (!data.is_object()) throw std::invalid_argument("update values must be an object");
    std::string sets;
    sql::Params params;
    bool has_updated_at = false;
    for (const auto& c : spec.columns) {
        sql::Value v;
        if (std::string_view(c.prop) == "updatedAt") {
            has_updated_at = true;
            v = to_driver(c, Json(static_cast<double>(now_ms())));
        } else {
            auto it = data.find(c.prop);
            if (it == data.end()) continue;
            v = to_driver(c, *it);
        }
        if (!sets.empty()) sets += ", ";
        sets += "\"";
        sets += c.column;
        sets += "\" = ?";
        params.add(std::move(v));
    }
    if (!has_updated_at) throw std::logic_error("table has no updatedAt");
    params.add(where_value);
    db.run(std::string("UPDATE \"") + spec.table + "\" SET " + sets + " WHERE \"" + where_column + "\" = ?", params);
}

template <class T>
std::vector<T> select_all(sql::Database& db, const std::string& sql, const sql::Params& params) {
    std::vector<T> out;
    for (const auto& row : db.all(sql, params, kRepositoryListLimits)) out.push_back(T::from_row(row));
    return out;
}

template <class T>
std::optional<T> select_one(sql::Database& db, const std::string& sql, const sql::Params& params) {
    auto row = db.get(sql, params);
    if (!row) return std::nullopt;
    return T::from_row(*row);
}

} // namespace

Repository::Repository(sql::DatabasePtr db) : db_(std::move(db)) {}

sql::Database& Repository::db() const {
    if (db_) return *db_;
    return *get_main_db();
}

// ── Users ─────────────────────────────────────────
std::optional<User> UserRepo::find_by_id(const std::string& id) const {
    return select_one<User>(db(), "SELECT * FROM users WHERE id = ?", {id});
}

void UserRepo::upsert(const UserUpsert& data) const {
    Json j = Json::object();
    j["id"] = data.id;
    j["login"] = data.login;
    j["displayName"] = data.displayName;
    j["email"] = data.email ? Json(*data.email) : Json(nullptr);
    j["avatarUrl"] = data.avatarUrl;
    j["role"] = data.role;
    if (find_by_id(data.id)) {
        update_by(db(), users_spec(), j, "id", data.id);
    } else {
        const double at = static_cast<double>(now_ms());
        j["createdAt"] = at;
        j["updatedAt"] = at;
        insert(db(), users_spec(), j, false);
    }
}

// ── Tasks ─────────────────────────────────────────
std::vector<Task> TaskRepo::find_by_workspace_id(const std::string& workspace_id) const {
    return select_all<Task>(db(), "SELECT * FROM tasks WHERE workspace_id = ? ORDER BY created_at DESC", {workspace_id});
}
std::vector<Task> TaskRepo::find_by_workspace_id_and_status(const std::string& workspace_id, const std::string& status) const {
    return select_all<Task>(db(), "SELECT * FROM tasks WHERE (workspace_id = ? AND status = ?) ORDER BY created_at DESC",
                            {workspace_id, status});
}
std::optional<Task> TaskRepo::find_by_id(const std::string& id) const {
    return select_one<Task>(db(), "SELECT * FROM tasks WHERE id = ?", {id});
}
std::vector<Task> TaskRepo::find_by_assignee(const std::string& assignee_id) const {
    return select_all<Task>(db(), "SELECT * FROM tasks WHERE assignee_id = ? ORDER BY created_at DESC", {assignee_id});
}
void TaskRepo::create(const Json& data) const { insert(db(), tasks_spec(), data, false); }
void TaskRepo::update(const std::string& id, const Json& data) const { update_by(db(), tasks_spec(), data, "id", id); }
void TaskRepo::delete_by_id(const std::string& id) const { db().run("DELETE FROM tasks WHERE id = ?", {id}); }

// ── Task Logs ─────────────────────────────────────
std::vector<TaskLog> TaskLogRepo::find_by_task_id(const std::string& task_id) const {
    return select_all<TaskLog>(db(), "SELECT * FROM task_logs WHERE task_id = ? ORDER BY created_at DESC", {task_id});
}
void TaskLogRepo::create(const Json& data) const { insert(db(), task_logs_spec(), data, false); }

// ── Channel Monitors ──────────────────────────────
std::vector<ChannelMonitor> MonitorRepo::find_by_workspace_id(const std::string& workspace_id) const {
    return select_all<ChannelMonitor>(db(), "SELECT * FROM channel_monitors WHERE workspace_id = ?", {workspace_id});
}
std::vector<ChannelMonitor> MonitorRepo::find_active_by_workspace_id(const std::string& workspace_id) const {
    return select_all<ChannelMonitor>(db(), "SELECT * FROM channel_monitors WHERE (workspace_id = ? AND is_active = ?)", {workspace_id, 1});
}
std::optional<ChannelMonitor> MonitorRepo::find_by_id(const std::string& id) const {
    return select_one<ChannelMonitor>(db(), "SELECT * FROM channel_monitors WHERE id = ?", {id});
}
void MonitorRepo::create(const Json& data) const { insert(db(), monitors_spec(), data, false); }
void MonitorRepo::update(const std::string& id, const Json& data) const { update_by(db(), monitors_spec(), data, "id", id); }
void MonitorRepo::delete_by_id(const std::string& id) const { db().run("DELETE FROM channel_monitors WHERE id = ?", {id}); }

// ── Chat Messages ─────────────────────────────────
std::vector<ChatMessage> ChatMessageRepo::find_by_monitor_id(const std::string& monitor_id, const ChatMessageQuery& opts) const {
    const double limit = opts.limit.value_or(100);
    if (opts.before) {
        return select_all<ChatMessage>(db(), "SELECT * FROM chat_messages WHERE (monitor_id = ? AND posted_at <= ?) ORDER BY posted_at DESC LIMIT ?",
                                       {monitor_id, *opts.before, limit});
    }
    return select_all<ChatMessage>(db(), "SELECT * FROM chat_messages WHERE monitor_id = ? ORDER BY posted_at DESC LIMIT ?",
                                   {monitor_id, limit});
}
std::vector<ChatMessage> ChatMessageRepo::find_by_monitor_id_in_range(const std::string& monitor_id, std::int64_t start, std::int64_t end) const {
    return select_all<ChatMessage>(db(), "SELECT * FROM chat_messages WHERE (monitor_id = ? AND posted_at >= ? AND posted_at <= ?) ORDER BY posted_at",
                                   {monitor_id, start, end});
}
bool ChatMessageRepo::exists_by_message_id(const std::string& monitor_id, const std::string& message_id) const {
    return db().get("SELECT id FROM chat_messages WHERE (monitor_id = ? AND message_id = ?) LIMIT ?", {monitor_id, message_id, 1}).has_value();
}
void ChatMessageRepo::create(const Json& data) const { insert(db(), chat_messages_spec(), data, true); }
std::int64_t ChatMessageRepo::count_by_monitor_id(const std::string& monitor_id) const {
    // TS は id を全件取って length を数えていた。結果は同じ COUNT にする (行を溜めない)。
    auto row = db().get("SELECT COUNT(*) AS n FROM chat_messages WHERE monitor_id = ?", {monitor_id});
    return row ? row->i64("n") : 0;
}

// ── Chat Summaries ────────────────────────────────
std::vector<ChatSummary> ChatSummaryRepo::find_by_monitor_id(const std::string& monitor_id) const {
    return select_all<ChatSummary>(db(), "SELECT * FROM chat_summaries WHERE monitor_id = ? ORDER BY created_at DESC", {monitor_id});
}
std::optional<ChatSummary> ChatSummaryRepo::find_by_id(const std::string& id) const {
    return select_one<ChatSummary>(db(), "SELECT * FROM chat_summaries WHERE id = ?", {id});
}
void ChatSummaryRepo::create(const Json& data) const { insert(db(), chat_summaries_spec(), data, false); }
void ChatSummaryRepo::delete_by_id(const std::string& id) const { db().run("DELETE FROM chat_summaries WHERE id = ?", {id}); }

// ─── Ludus mechanics dictionary ───────────────────────────────────────────────
std::vector<LudusMechanic> LudusMechanicRepo::list_by_workspace(const std::string& workspace_id, double limit) const {
    return select_all<LudusMechanic>(db(), "SELECT * FROM ludus_mechanics WHERE workspace_id = ? ORDER BY created_at DESC LIMIT ?",
                                     {workspace_id, limit});
}
void LudusMechanicRepo::create(const Json& data) const { insert(db(), ludus_mechanics_spec(), data, true); }
std::vector<LudusMechanic> LudusMechanicRepo::search_by_game(const std::string& workspace_id, const std::string& game_title) const {
    return select_all<LudusMechanic>(
        db(), "SELECT * FROM ludus_mechanics WHERE (workspace_id = ? AND game_title = ?) ORDER BY confidence DESC, created_at DESC",
        {workspace_id, game_title});
}

// ─── Economy Graph Cache ──────────────────────────────────────────────────────
std::optional<LudusEconomyGraph> EconomyGraphRepo::find_by_game(const std::string& workspace_id, const std::string& game_title) const {
    return select_one<LudusEconomyGraph>(db(), "SELECT * FROM ludus_economy_graphs WHERE (workspace_id = ? AND game_title = ?)",
                                         {workspace_id, game_title});
}
void EconomyGraphRepo::upsert(const Json& data) const {
    if (!data.is_object()) throw std::invalid_argument("upsert values must be an object");
    const std::string ws = json_string(data, "workspaceId");
    const std::string title = json_string(data, "gameTitle");
    auto existing = find_by_game(ws, title);
    if (existing) {
        Json patch = Json::object();
        if (data.contains("graphJson")) patch["graphJson"] = data["graphJson"];
        if (data.contains("mechanicCount")) patch["mechanicCount"] = data["mechanicCount"];
        update_by(db(), economy_graphs_spec(), patch, "id", existing->id);
    } else {
        insert(db(), economy_graphs_spec(), data, false);
    }
}
std::vector<LudusEconomyGraph> EconomyGraphRepo::list_by_workspace(const std::string& workspace_id) const {
    return select_all<LudusEconomyGraph>(db(), "SELECT * FROM ludus_economy_graphs WHERE workspace_id = ? ORDER BY updated_at DESC",
                                         {workspace_id});
}

// ─── Learning jobs ────────────────────────────────────────────────────────────
void LudusLearningJobRepo::create(const Json& data) const { insert(db(), learning_jobs_spec(), data, false); }
void LudusLearningJobRepo::update(const std::string& id, const Json& data) const {
    update_by(db(), learning_jobs_spec(), data, "id", id);
}
std::optional<LudusLearningJob> LudusLearningJobRepo::find_by_id(const std::string& id) const {
    return select_one<LudusLearningJob>(db(), "SELECT * FROM ludus_learning_jobs WHERE id = ?", {id});
}
std::vector<LudusLearningJob> LudusLearningJobRepo::list_by_workspace(const std::string& workspace_id, double limit) const {
    return select_all<LudusLearningJob>(db(), "SELECT * FROM ludus_learning_jobs WHERE workspace_id = ? ORDER BY created_at DESC LIMIT ?",
                                        {workspace_id, limit});
}

} // namespace di::db
