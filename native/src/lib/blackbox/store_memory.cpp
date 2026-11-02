#include "lib/blackbox/store_memory.hpp"

#include <algorithm>

#include "base/crypto.hpp"
#include "base/time.hpp"
#include "lib/blackbox/fingerprint.hpp"

namespace di::lib::blackbox {

Rule make_rule_from_draft(const RuleDraft& draft, const std::string& id, const std::string& ts) {
    Rule r;
    r.id = id;
    r.domain = draft.domain;
    r.description = draft.description;
    r.when = draft.when;
    r.output = draft.output;  // undefined ?? null は Json の null
    r.confidence = draft.confidence.value_or(0.7);
    r.state = draft.state.value_or("candidate");
    r.source = draft.source.value_or("manual");
    r.fingerprint = rule_fingerprint(draft.when, draft.output);
    r.priority = draft.priority.value_or(0);
    r.createdAt = ts;
    r.updatedAt = ts;
    return r;
}

DecisionRecord make_record(const DecisionInput& rec, std::int64_t id) {
    DecisionRecord r;
    r.id = id;
    r.domain = rec.domain;
    r.input = rec.input;
    r.features = rec.features;
    r.output = rec.output;
    r.source = rec.source;
    r.ruleId = rec.ruleId;
    r.confidence = rec.confidence;
    r.rationale = rec.rationale;
    r.status = rec.status;
    r.shadow = rec.shadow;
    r.createdAt = rec.createdAt;
    return r;
}

std::vector<DecisionRecord> tail_reversed(std::vector<DecisionRecord> items, std::int64_t limit) {
    // JS の slice(-limit): limit <= 0 は slice(-0)=slice(0) で全件 (負数は先頭からの位置)。
    std::size_t start = 0;
    if (limit > 0) {
        const auto n = static_cast<std::size_t>(limit);
        start = n >= items.size() ? 0 : items.size() - n;
    } else if (limit < 0) {
        const auto n = static_cast<std::size_t>(-limit);
        start = std::min(n, items.size());
    }
    std::vector<DecisionRecord> out(items.begin() + static_cast<std::ptrdiff_t>(start), items.end());
    std::reverse(out.begin(), out.end());
    return out;
}

void sort_rules(std::vector<Rule>& rules) {
    std::stable_sort(rules.begin(), rules.end(), [](const Rule& a, const Rule& b) {
        if (a.priority != b.priority) return b.priority < a.priority;
        return a.createdAt < b.createdAt;
    });
}

MemoryRuleStore::MemoryRuleStore(std::function<std::string()> now)
    : now_(now ? std::move(now) : std::function<std::string()>([] { return time::now_iso(); })) {}

std::vector<Rule> MemoryRuleStore::list_by_domain(const std::string& domain) {
    std::lock_guard lock(mutex_);
    std::vector<Rule> out;
    for (const auto& r : rules_)
        if (r.domain == domain) out.push_back(r);
    sort_rules(out);
    return out;
}

std::optional<Rule> MemoryRuleStore::find_by_fingerprint(const std::string& domain, const std::string& fingerprint) {
    std::lock_guard lock(mutex_);
    for (const auto& r : rules_)
        if (r.domain == domain && r.fingerprint == fingerprint) return r;
    return std::nullopt;
}

std::optional<Rule> MemoryRuleStore::get(const std::string& id) {
    std::lock_guard lock(mutex_);
    for (const auto& r : rules_)
        if (r.id == id) return r;
    return std::nullopt;
}

Rule MemoryRuleStore::insert(const RuleDraft& draft) {
    Rule rule = make_rule_from_draft(draft, crypto::uuid_v4(), now_());
    std::lock_guard lock(mutex_);
    rules_.push_back(rule);
    return rule;
}

std::optional<Rule> MemoryRuleStore::update(const std::string& id, const RulePatch& patch) {
    std::lock_guard lock(mutex_);
    for (auto& r : rules_) {
        if (r.id != id) continue;
        apply_patch(r, patch);
        r.updatedAt = now_();
        return r;
    }
    return std::nullopt;
}

std::vector<Rule> MemoryRuleStore::snapshot() {
    std::lock_guard lock(mutex_);
    return rules_;
}

void MemoryRuleStore::restore(const std::vector<Rule>& rules) {
    std::lock_guard lock(mutex_);
    // new Map(rules.map(r => [r.id, r])): 同じ id は後勝ち (位置は最初の出現)。
    rules_.clear();
    for (const auto& r : rules) {
        auto it = std::find_if(rules_.begin(), rules_.end(), [&](const Rule& x) { return x.id == r.id; });
        if (it != rules_.end()) *it = r;
        else rules_.push_back(r);
    }
}

std::int64_t MemoryDecisionLedger::record(const DecisionInput& rec) {
    std::lock_guard lock(mutex_);
    const std::int64_t id = ++seq_;
    records_.push_back(make_record(rec, id));
    return id;
}

std::optional<DecisionRecord> MemoryDecisionLedger::get(std::int64_t id) {
    std::lock_guard lock(mutex_);
    for (const auto& r : records_)
        if (r.id == id) return r;
    return std::nullopt;
}

void MemoryDecisionLedger::set_verdict(std::int64_t id, const std::string& verdict, const std::string& reviewed_at) {
    std::lock_guard lock(mutex_);
    for (auto& r : records_) {
        if (r.id != id) continue;
        r.verdict = verdict;
        r.reviewedAt = reviewed_at;
        return;
    }
}

std::vector<DecisionRecord> MemoryDecisionLedger::list_pending(const std::optional<std::string>& domain, std::int64_t limit) {
    std::lock_guard lock(mutex_);
    std::vector<DecisionRecord> f;
    for (const auto& r : records_)
        if (r.status == "pending_review" && !r.verdict && (!domain || domain->empty() || r.domain == *domain)) f.push_back(r);
    return tail_reversed(std::move(f), limit);
}

std::vector<DecisionRecord> MemoryDecisionLedger::list_recent(const std::string& domain, std::int64_t limit) {
    std::lock_guard lock(mutex_);
    std::vector<DecisionRecord> f;
    for (const auto& r : records_)
        if (r.domain == domain) f.push_back(r);
    return tail_reversed(std::move(f), limit);
}

} // namespace di::lib::blackbox
