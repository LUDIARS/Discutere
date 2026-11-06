#pragma once
// インメモリ実装の RuleStore / DecisionLedger (store-memory.ts)。
// 用途: テスト、ゲームランタイム、永続化不要なプロトタイプ。直列化は snapshot()/restore()。
// 複数スレッドから使えるよう内部で mutex を握る。
#include <functional>
#include <map>
#include <mutex>
#include <vector>

#include "lib/blackbox/types.hpp"

namespace di::lib::blackbox {

class MemoryRuleStore : public RuleStore {
public:
    explicit MemoryRuleStore(std::function<std::string()> now = {});
    std::vector<Rule> list_by_domain(const std::string& domain) override;
    std::optional<Rule> find_by_fingerprint(const std::string& domain, const std::string& fingerprint) override;
    std::optional<Rule> get(const std::string& id) override;
    Rule insert(const RuleDraft& draft) override;
    std::optional<Rule> update(const std::string& id, const RulePatch& patch) override;
    /** 直列化用スナップショット (挿入順)。 */
    std::vector<Rule> snapshot();
    void restore(const std::vector<Rule>& rules);

private:
    std::function<std::string()> now_;
    std::mutex mutex_;
    std::vector<Rule> rules_;  // Map の挿入順を保つ
};

class MemoryDecisionLedger : public DecisionLedger {
public:
    std::int64_t record(const DecisionInput& rec) override;
    std::optional<DecisionRecord> get(std::int64_t id) override;
    void set_verdict(std::int64_t id, const std::string& verdict, const std::string& reviewed_at) override;
    std::vector<DecisionRecord> list_pending(const std::optional<std::string>& domain = std::nullopt,
                                             std::int64_t limit = 50) override;
    std::vector<DecisionRecord> list_recent(const std::string& domain, std::int64_t limit) override;

private:
    std::mutex mutex_;
    std::vector<DecisionRecord> records_;
    std::int64_t seq_ = 0;
};

/** RuleDraft から新しい Rule を組む (memory / jsonfile 共通)。 */
Rule make_rule_from_draft(const RuleDraft& draft, const std::string& id, const std::string& ts);
/** DecisionInput から DecisionRecord を組む ({ ...rec, id, verdict: null, reviewedAt: null })。 */
DecisionRecord make_record(const DecisionInput& rec, std::int64_t id);
/** 配列の末尾 limit 件を逆順で返す (.slice(-limit).reverse())。 */
std::vector<DecisionRecord> tail_reversed(std::vector<DecisionRecord> items, std::int64_t limit);
/** list_by_domain の並び (priority 降順 → createdAt 昇順、安定)。 */
void sort_rules(std::vector<Rule>& rules);

} // namespace di::lib::blackbox
