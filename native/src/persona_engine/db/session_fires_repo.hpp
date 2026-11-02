#pragma once
// per-session fire counter store (src/persona-engine/db/session-fires-repo.ts, restart-recovery)。
// safety cap (maxFiresPerSession / maxFiresPerRulePerSession) を再起動越しに効かせるため SQLite に逃がす。
// - InMemorySessionFireStore: 従来挙動 (テスト / db 無し時の既定)。
// - SqliteSessionFireStore: session_rule_fires テーブルに永続化 (本番)。
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

#include "base/sqlite.hpp"

namespace di::persona_engine::db {

class SessionFireStore {
public:
    virtual ~SessionFireStore() = default;
    /** session 内の総発火数 */
    virtual std::int64_t total(const std::string& session_id) = 0;
    /** session 内の特定 rule の発火数 */
    virtual std::int64_t rule_count(const std::string& session_id, const std::string& rule_id) = 0;
    /** 1 件発火を記録 (count + 1) */
    virtual void increment(const std::string& session_id, const std::string& rule_id) = 0;
    /** session のカウンタを全消去 (議論クローズ時の cap 解放) */
    virtual void reset(const std::string& session_id) = 0;
};

class InMemorySessionFireStore : public SessionFireStore {
public:
    std::int64_t total(const std::string& session_id) override;
    std::int64_t rule_count(const std::string& session_id, const std::string& rule_id) override;
    void increment(const std::string& session_id, const std::string& rule_id) override;
    void reset(const std::string& session_id) override;

private:
    std::mutex mutex_;
    std::map<std::string, std::map<std::string, std::int64_t>> fires_;
};

class SqliteSessionFireStore : public SessionFireStore {
public:
    explicit SqliteSessionFireStore(sql::DatabasePtr db);
    std::int64_t total(const std::string& session_id) override;
    std::int64_t rule_count(const std::string& session_id, const std::string& rule_id) override;
    void increment(const std::string& session_id, const std::string& rule_id) override;
    void reset(const std::string& session_id) override;

private:
    sql::DatabasePtr db_;
};

} // namespace di::persona_engine::db
