#pragma once
// @ludiars/blackbox — 成長型ブラックボックスの公開 API (index.ts)。
#include <memory>
#include <string>

#include "base/sqlite.hpp"
#include "lib/blackbox/condition.hpp"
#include "lib/blackbox/engine.hpp"
#include "lib/blackbox/fingerprint.hpp"
#include "lib/blackbox/lifecycle.hpp"
#include "lib/blackbox/stats.hpp"
#include "lib/blackbox/store_memory.hpp"
#include "lib/blackbox/store_sqlite.hpp"
#include "lib/blackbox/types.hpp"

namespace di::lib::blackbox {

struct BlackBox {
    std::shared_ptr<BlackBoxEngine> engine;
    std::shared_ptr<RuleStore> rules;
    std::shared_ptr<DecisionLedger> ledger;
    /** domain の卒業メトリクス (直近 window 件、既定 100)。 */
    DomainStats stats(const std::string& domain, std::int64_t window = 100) const {
        return domain_stats(domain, ledger->list_recent(domain, window), rules->list_by_domain(domain));
    }
};

/** SQLite で束ねた blackbox を 1 つ作る。schema も保証する。 */
BlackBox make_sqlite_blackbox(const sql::DatabasePtr& db, const EngineOptions& opts = {});
/** インメモリで束ねた blackbox を 1 つ作る (テスト / ゲームランタイム)。 */
BlackBox make_memory_blackbox(const EngineOptions& opts = {});

} // namespace di::lib::blackbox
