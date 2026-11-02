#include "persona_engine/db/session_fires_repo.hpp"

namespace di::persona_engine::db {

std::int64_t InMemorySessionFireStore::total(const std::string& session_id) {
    std::lock_guard lock(mutex_);
    auto it = fires_.find(session_id);
    if (it == fires_.end()) return 0;
    std::int64_t sum = 0;
    for (const auto& [_, v] : it->second) sum += v;
    return sum;
}

std::int64_t InMemorySessionFireStore::rule_count(const std::string& session_id, const std::string& rule_id) {
    std::lock_guard lock(mutex_);
    auto it = fires_.find(session_id);
    if (it == fires_.end()) return 0;
    auto r = it->second.find(rule_id);
    return r == it->second.end() ? 0 : r->second;
}

void InMemorySessionFireStore::increment(const std::string& session_id, const std::string& rule_id) {
    std::lock_guard lock(mutex_);
    fires_[session_id][rule_id] += 1;
}

void InMemorySessionFireStore::reset(const std::string& session_id) {
    std::lock_guard lock(mutex_);
    fires_.erase(session_id);
}

SqliteSessionFireStore::SqliteSessionFireStore(sql::DatabasePtr db) : db_(std::move(db)) {}

std::int64_t SqliteSessionFireStore::total(const std::string& session_id) {
    auto row = db_->get("SELECT COALESCE(SUM(count), 0) AS total FROM session_rule_fires WHERE session_id = ?", {session_id});
    return row ? row->i64("total") : 0;
}

std::int64_t SqliteSessionFireStore::rule_count(const std::string& session_id, const std::string& rule_id) {
    auto row = db_->get("SELECT count FROM session_rule_fires WHERE session_id = ? AND rule_id = ?", {session_id, rule_id});
    return row ? row->i64("count") : 0;
}

void SqliteSessionFireStore::increment(const std::string& session_id, const std::string& rule_id) {
    db_->run(R"(INSERT INTO session_rule_fires (session_id, rule_id, count) VALUES (?, ?, 1)
         ON CONFLICT(session_id, rule_id) DO UPDATE SET count = count + 1)",
             {session_id, rule_id});
}

void SqliteSessionFireStore::reset(const std::string& session_id) {
    db_->run("DELETE FROM session_rule_fires WHERE session_id = ?", {session_id});
}

} // namespace di::persona_engine::db
