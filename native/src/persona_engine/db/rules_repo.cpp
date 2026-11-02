#include "persona_engine/db/rules_repo.hpp"

#include "base/strings.hpp"
#include "base/time.hpp"

namespace di::persona_engine::db {

namespace {
std::int64_t now_sec() { return time::now_ms() / 1000; }
} // namespace

RulesRepo::RulesRepo(sql::DatabasePtr db) : db_(std::move(db)) {}

void RulesRepo::insert_or_ignore(const RuleSeed& seed, const std::optional<std::string>& added_by) {
    const auto now = now_sec();
    db_->run(R"(INSERT OR IGNORE INTO rules
          (id, description, trigger_type, tick_sec, event_kind, conditions,
           instructions, target, cooldown_sec, last_fired_at, enabled,
           added_at, added_by)
         VALUES (?, ?, ?, ?, ?, '[]', ?, ?, ?, NULL, 1, ?, ?))",
             {seed.id, seed.description, seed.trigger_type, seed.tick_sec, seed.event_kind, seed.instructions, seed.target,
              seed.cooldown_sec.value_or(60), now, added_by.value_or("seed")});
}

void RulesRepo::reconcile_seed(const RuleSeed& seed) {
    db_->run(R"(UPDATE rules SET
           trigger_type = ?, tick_sec = ?, event_kind = ?, target = ?, cooldown_sec = ?
         WHERE id = ?)",
             {seed.trigger_type, seed.tick_sec, seed.event_kind, seed.target, seed.cooldown_sec.value_or(60), seed.id});
}

void RulesRepo::bulk_seed(const std::vector<RuleSeed>& seeds) {
    db_->transaction([&] {
        for (const auto& row : seeds) {
            insert_or_ignore(row);
            // INSERT OR IGNORE で既存行は据え置かれるため、発火パラメータを seed に追従。
            reconcile_seed(row);
        }
    });
}

std::optional<RuleRow> RulesRepo::get(const std::string& id) {
    auto row = db_->get("SELECT * FROM rules WHERE id = ?", {id});
    if (!row) return std::nullopt;
    return rule_row_from(*row);
}

std::vector<RuleRow> RulesRepo::list(const RuleListFilter& filter) {
    std::vector<std::string> where;
    sql::Params params;
    if (filter.enabled) {
        where.push_back("enabled = ?");
        params.add(*filter.enabled ? 1 : 0);
    }
    if (filter.trigger_type) {
        where.push_back("trigger_type = ?");
        params.add(*filter.trigger_type);
    }
    const std::string sql =
        "SELECT * FROM rules" + (where.empty() ? std::string() : " WHERE " + str::join(where, " AND ")) + " ORDER BY id";
    std::vector<RuleRow> out;
    for (const auto& row : db_->all(sql, params)) out.push_back(rule_row_from(row));
    return out;
}

void RulesRepo::set_last_fired(const std::string& id, std::int64_t at_sec) {
    db_->run("UPDATE rules SET last_fired_at = ? WHERE id = ?", {at_sec, id});
}

void RulesRepo::set_enabled(const std::string& id, bool enabled) {
    db_->run("UPDATE rules SET enabled = ? WHERE id = ?", {enabled ? 1 : 0, id});
}

void RulesRepo::remove(const std::string& id, const std::string& by, const std::string& reason) {
    db_->run("UPDATE rules SET enabled = 0, removed_at = ?, removed_by = ?, removed_reason = ? WHERE id = ?",
             {now_sec(), by, reason, id});
}

void RulesRepo::log(const RuleLogInput& input) {
    db_->run("INSERT INTO rule_log (ts, rule_id, action, actor, detail) VALUES (?, ?, ?, ?, ?)",
             {now_sec(), input.rule_id, input.action, input.actor, input.detail});
}

std::vector<RuleLogRow> RulesRepo::recent_logs(std::int64_t limit) {
    std::vector<RuleLogRow> out;
    for (const auto& row : db_->all("SELECT * FROM rule_log ORDER BY id DESC LIMIT ?", {limit})) out.push_back(rule_log_row_from(row));
    return out;
}

} // namespace di::persona_engine::db
