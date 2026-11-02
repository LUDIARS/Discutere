#pragma once
// src/persona-engine/db/rules-repo.ts — rule CRUD + seed の発火パラメータ追従 + rule_log。
#include <optional>
#include <string>
#include <vector>

#include "base/sqlite.hpp"
#include "persona_engine/types.hpp"

namespace di::persona_engine::db {

struct RuleListFilter {
    std::optional<bool> enabled;
    /** "tick" | "event" */
    std::optional<std::string> trigger_type;
};

struct RuleLogInput {
    std::optional<std::string> rule_id;
    /** RuleLogRow::action */
    std::string action;
    /** RuleLogRow::actor */
    std::string actor;
    std::optional<std::string> detail;
};

class RulesRepo {
public:
    explicit RulesRepo(sql::DatabasePtr db);

    void insert_or_ignore(const RuleSeed& seed, const std::optional<std::string>& added_by = std::nullopt);
    /**
     * seed rule の「発火制御フィールド」(trigger_type / tick_sec / event_kind / target / cooldown_sec) を
     * 現行 seed 定義に合わせて更新する (INSERT OR IGNORE では既存行が更新されないため、boot 時に self-heal)。
     * runtime 状態 (last_fired_at / enabled / removed_*) と instructions は保持する。
     */
    void reconcile_seed(const RuleSeed& seed);
    void bulk_seed(const std::vector<RuleSeed>& seeds);
    std::optional<RuleRow> get(const std::string& id);
    std::vector<RuleRow> list(const RuleListFilter& filter = {});
    void set_last_fired(const std::string& id, std::int64_t at_sec);
    void set_enabled(const std::string& id, bool enabled);
    void remove(const std::string& id, const std::string& by, const std::string& reason);

    void log(const RuleLogInput& input);
    std::vector<RuleLogRow> recent_logs(std::int64_t limit = 20);

private:
    sql::DatabasePtr db_;
};

} // namespace di::persona_engine::db
