#pragma once
// Discutere — リポジトリ層 (src/db/repository.ts の drizzle クエリを生 SQL で書き直したもの)。
//
// 値の渡し方: insert / update は TS の `$inferInsert` と同じプロパティ名 (camelCase) の JSON object で受ける
// (drizzle と同じく表に無いキーは無視、undefined に当たるキーは渡さない)。列の型ごとの変換:
//   text → 文字列 (null は NULL)
//   integer → 数 (整数に丸めずにそのまま。drizzle と同じ)
//   integer {mode:"boolean"} → true/false を 1/0
//   integer {mode:"timestamp"} → エポックミリ秒 (JS の Date#getTime) を受け、秒に切り捨てて保存 (drizzle と同じ)
//   integer {mode:"timestamp_ms"} → エポックミリ秒をそのまま
// insert で省略した列は drizzle と同じく .default(x) の値、$defaultFn(() => new Date()) の列は現在時刻。
// update は渡したキーだけを SET し、updatedAt を現在時刻にする (`.set({ ...data, updatedAt: new Date() })`)。
// 返す行は db/schema.hpp の行 struct (timestamp は Unix 秒、timestamp_ms はミリ秒)。
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "base/json.hpp"
#include "base/sqlite.hpp"
#include "db/schema.hpp"

namespace di::db {

/** 一覧の行数・バイト数の上限 (DB の値も外部入力として課金する)。 */
inline constexpr sql::QueryLimits kRepositoryListLimits{200'000, 256u << 20};

/** ludusMechanicRepo 等のリポジトリが使う接続 (既定 get_main_db())。 */
class Repository {
public:
    explicit Repository(sql::DatabasePtr db = nullptr);
    sql::Database& db() const;

private:
    sql::DatabasePtr db_;
};

/** 表の値の変換を誤ったとき (型の合わない値) は std::invalid_argument。 */
struct UserUpsert {
    std::string id;
    std::string login;
    std::string displayName;
    std::optional<std::string> email;
    std::string avatarUrl;
    std::string role;
};

class UserRepo : public Repository {
public:
    using Repository::Repository;
    std::optional<User> find_by_id(const std::string& id) const;
    void upsert(const UserUpsert& data) const;
};

class TaskRepo : public Repository {
public:
    using Repository::Repository;
    std::vector<Task> find_by_workspace_id(const std::string& workspace_id) const;
    std::vector<Task> find_by_workspace_id_and_status(const std::string& workspace_id, const std::string& status) const;
    std::optional<Task> find_by_id(const std::string& id) const;
    std::vector<Task> find_by_assignee(const std::string& assignee_id) const;
    void create(const Json& data) const;
    void update(const std::string& id, const Json& data) const;
    void delete_by_id(const std::string& id) const;
};

class TaskLogRepo : public Repository {
public:
    using Repository::Repository;
    std::vector<TaskLog> find_by_task_id(const std::string& task_id) const;
    void create(const Json& data) const;
};

class MonitorRepo : public Repository {
public:
    using Repository::Repository;
    std::vector<ChannelMonitor> find_by_workspace_id(const std::string& workspace_id) const;
    std::vector<ChannelMonitor> find_active_by_workspace_id(const std::string& workspace_id) const;
    std::optional<ChannelMonitor> find_by_id(const std::string& id) const;
    void create(const Json& data) const;
    void update(const std::string& id, const Json& data) const;
    void delete_by_id(const std::string& id) const;
};

struct ChatMessageQuery {
    std::optional<double> limit;
    /** エポックミリ秒 (posted_at <= before)。 */
    std::optional<std::int64_t> before;
};

class ChatMessageRepo : public Repository {
public:
    using Repository::Repository;
    std::vector<ChatMessage> find_by_monitor_id(const std::string& monitor_id, const ChatMessageQuery& opts = {}) const;
    /** start / end はエポックミリ秒。 */
    std::vector<ChatMessage> find_by_monitor_id_in_range(const std::string& monitor_id, std::int64_t start, std::int64_t end) const;
    bool exists_by_message_id(const std::string& monitor_id, const std::string& message_id) const;
    /** ON CONFLICT DO NOTHING。 */
    void create(const Json& data) const;
    std::int64_t count_by_monitor_id(const std::string& monitor_id) const;
};

class ChatSummaryRepo : public Repository {
public:
    using Repository::Repository;
    std::vector<ChatSummary> find_by_monitor_id(const std::string& monitor_id) const;
    std::optional<ChatSummary> find_by_id(const std::string& id) const;
    void create(const Json& data) const;
    void delete_by_id(const std::string& id) const;
};

class LudusMechanicRepo : public Repository {
public:
    using Repository::Repository;
    std::vector<LudusMechanic> list_by_workspace(const std::string& workspace_id, double limit = 200) const;
    /** ON CONFLICT DO NOTHING。 */
    void create(const Json& data) const;
    std::vector<LudusMechanic> search_by_game(const std::string& workspace_id, const std::string& game_title) const;
};

class EconomyGraphRepo : public Repository {
public:
    using Repository::Repository;
    std::optional<LudusEconomyGraph> find_by_game(const std::string& workspace_id, const std::string& game_title) const;
    /** 既存 (workspaceId, gameTitle) があれば graphJson / mechanicCount / updatedAt だけ更新、無ければ insert。 */
    void upsert(const Json& data) const;
    std::vector<LudusEconomyGraph> list_by_workspace(const std::string& workspace_id) const;
};

class LudusLearningJobRepo : public Repository {
public:
    using Repository::Repository;
    void create(const Json& data) const;
    void update(const std::string& id, const Json& data) const;
    std::optional<LudusLearningJob> find_by_id(const std::string& id) const;
    std::vector<LudusLearningJob> list_by_workspace(const std::string& workspace_id, double limit = 50) const;
};

} // namespace di::db
