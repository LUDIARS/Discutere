// @implements DI-EXTERNAL-DISCUSSION
#include "external_discussion/store.hpp"

#include "base/crypto.hpp"

namespace di::external_discussion {

namespace {
/** 保存済みの結果 JSON の上限 (DB の値も外部入力として扱う)。 */
const JsonLimits kResultLimits{64 * 1024, 1'000, 8};
ParticipationResult status_only(const char* status) {
    ParticipationResult r;
    r.status = status;
    return r;
}
}  // namespace

ParticipationStore::ParticipationStore(sql::DatabasePtr db) : db_(std::move(db)) {
    db_->exec(R"(CREATE TABLE IF NOT EXISTS external_discussion_attempts (
      id TEXT PRIMARY KEY, scene TEXT NOT NULL, state TEXT NOT NULL,
      result TEXT, human_id TEXT NOT NULL, owner TEXT NOT NULL, lease_until INTEGER NOT NULL, created_at INTEGER NOT NULL);
      CREATE INDEX IF NOT EXISTS external_discussion_scene ON external_discussion_attempts(scene,created_at);)");
}

ClaimResult ParticipationStore::claim(const std::string& scene, const std::string& id, const std::string& human_id,
                                      std::int64_t now) {
    auto body = [&]() -> ClaimResult {
        auto row = db_->get("SELECT * FROM external_discussion_attempts WHERE id=?", {id});
        if (row && row->str("state") == "done") {
            const std::string raw = row->opt_str("result").value_or("{}");
            return {participation_result_from_json(parse_json_or_throw(raw, kResultLimits)), std::nullopt};
        }
        if (db_->get("SELECT id FROM external_discussion_attempts WHERE scene=? AND human_id=? AND state='done' AND result LIKE '%\"status\":\"proposal\"%'",
                     {scene, human_id}))
            return {status_only("skipped"), std::nullopt};
        if (row && row->i64("lease_until") > now) return {status_only("busy"), std::nullopt};
        if (db_->get("SELECT id FROM external_discussion_attempts WHERE scene=? AND state='running' AND lease_until>?", {scene, now}))
            return {status_only("busy"), std::nullopt};
        if (db_->get("SELECT id FROM external_discussion_attempts WHERE scene=? AND state='done' AND result LIKE '%\"status\":\"proposal\"%' AND created_at>?",
                     {scene, now - 180'000}))
            return {status_only("waiting"), std::nullopt};
        const std::string token = crypto::uuid_v4();
        db_->run(R"(INSERT INTO external_discussion_attempts(id,scene,state,result,human_id,owner,lease_until,created_at)
        VALUES(?,?,'running',NULL,?,?,?,?) ON CONFLICT(id) DO UPDATE SET state='running',owner=excluded.owner,lease_until=excluded.lease_until)",
                 {id, scene, human_id, token, now + 180'000, now});
        return {std::nullopt, token};
    };
    // better-sqlite3 の transaction(fn).immediate() と同じ: 外側なら BEGIN IMMEDIATE、入れ子なら SAVEPOINT。
    std::lock_guard lock(db_->mutex());
    if (db_->in_transaction()) return db_->transaction(body);
    db_->exec("BEGIN IMMEDIATE");
    try {
        ClaimResult r = body();
        db_->exec("COMMIT");
        return r;
    } catch (...) {
        try {
            db_->exec("ROLLBACK");
        } catch (...) {
        }
        throw;
    }
}

void ParticipationStore::finish(const std::string& id, const std::string& token, const ParticipationResult& result) {
    const auto update = db_->run("UPDATE external_discussion_attempts SET state='done',result=?,lease_until=0 WHERE id=? AND owner=? AND state='running'",
                                 {json_stringify(to_json(result)), id, token});
    if (update.changes != 1) throw std::runtime_error("discussion lease lost");
}

void ParticipationStore::fail(const std::string& id, const std::string& token) {
    db_->run("UPDATE external_discussion_attempts SET state='failed',lease_until=0 WHERE id=? AND owner=?", {id, token});
}

} // namespace di::external_discussion
