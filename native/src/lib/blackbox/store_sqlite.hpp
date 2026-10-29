#pragma once
// SQLite 実装の RuleStore / DecisionLedger (store-sqlite.ts)。
// ensure_blackbox_schema() は新規作成に加えて、Memoria 旧 schema (enabled 列 / state 無し) からの
// 追い付き migration も行う。DDL・SQL は TS と同じ文 (既存 DB ファイルをそのまま読める)。
#include <functional>

#include "base/sqlite.hpp"
#include "lib/blackbox/types.hpp"

namespace di::lib::blackbox {

/** テーブルを作成し、旧 schema なら列を足して state を埋める。何度呼んでも冪等。 */
void ensure_blackbox_schema(sql::Database& db);

class SqliteRuleStore : public RuleStore {
public:
    explicit SqliteRuleStore(sql::DatabasePtr db, std::function<std::string()> now = {});
    std::vector<Rule> list_by_domain(const std::string& domain) override;
    std::optional<Rule> find_by_fingerprint(const std::string& domain, const std::string& fingerprint) override;
    std::optional<Rule> get(const std::string& id) override;
    Rule insert(const RuleDraft& draft) override;
    std::optional<Rule> update(const std::string& id, const RulePatch& patch) override;

private:
    sql::DatabasePtr db_;
    std::function<std::string()> now_;
};

class SqliteDecisionLedger : public DecisionLedger {
public:
    explicit SqliteDecisionLedger(sql::DatabasePtr db);
    std::int64_t record(const DecisionInput& rec) override;
    std::optional<DecisionRecord> get(std::int64_t id) override;
    void set_verdict(std::int64_t id, const std::string& verdict, const std::string& reviewed_at) override;
    std::vector<DecisionRecord> list_pending(const std::optional<std::string>& domain = std::nullopt,
                                             std::int64_t limit = 50) override;
    std::vector<DecisionRecord> list_recent(const std::string& domain, std::int64_t limit) override;

private:
    sql::DatabasePtr db_;
};

/** DB の JSON 列を読む上限 (1 列あたり)。 */
JsonLimits db_json_limits();

} // namespace di::lib::blackbox
