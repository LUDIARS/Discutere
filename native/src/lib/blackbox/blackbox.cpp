#include "lib/blackbox/blackbox.hpp"

namespace di::lib::blackbox {

namespace {
BlackBox bundle(std::shared_ptr<RuleStore> rules, std::shared_ptr<DecisionLedger> ledger, const EngineOptions& opts) {
    BlackBox bb;
    bb.engine = std::make_shared<BlackBoxEngine>(rules, ledger, opts);
    bb.rules = std::move(rules);
    bb.ledger = std::move(ledger);
    return bb;
}
} // namespace

BlackBox make_sqlite_blackbox(const sql::DatabasePtr& db, const EngineOptions& opts) {
    ensure_blackbox_schema(*db);
    return bundle(std::make_shared<SqliteRuleStore>(db, opts.now), std::make_shared<SqliteDecisionLedger>(db), opts);
}

BlackBox make_memory_blackbox(const EngineOptions& opts) {
    return bundle(std::make_shared<MemoryRuleStore>(opts.now), std::make_shared<MemoryDecisionLedger>(), opts);
}

} // namespace di::lib::blackbox
