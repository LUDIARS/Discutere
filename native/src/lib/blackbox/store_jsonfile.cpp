#include "lib/blackbox/store_jsonfile.hpp"

#include <algorithm>
#include <stdexcept>

#include "base/crypto.hpp"
#include "base/fsutil.hpp"
#include "base/log.hpp"
#include "base/time.hpp"

namespace di::lib::blackbox {

namespace {
std::function<std::string()> or_now(std::function<std::string()> now) {
    if (now) return now;
    return [] { return time::now_iso(); };
}
JsonLimits file_limits() {
    JsonLimits l;
    l.max_bytes = kBlackboxFileMaxBytes;
    l.max_elements = 4'000'000;
    l.max_depth = 64;
    return l;
}
} // namespace

Json decision_record_to_file_json(const DecisionRecord& r) {
    Json shadow = Json::array();
    for (const auto& e : r.shadow) shadow.push_back(to_json(e));
    return Json{{"domain", r.domain},
                {"input", r.input},
                {"features", r.features},
                {"output", r.output},
                {"source", r.source},
                {"ruleId", r.ruleId ? Json(*r.ruleId) : Json(nullptr)},
                {"confidence", r.confidence},
                {"rationale", r.rationale},
                {"status", r.status},
                {"shadow", std::move(shadow)},
                {"createdAt", r.createdAt},
                {"id", r.id},
                {"verdict", r.verdict ? Json(*r.verdict) : Json(nullptr)},
                {"reviewedAt", r.reviewedAt ? Json(*r.reviewedAt) : Json(nullptr)}};
}

BlackboxJsonFile::BlackboxJsonFile(std::string path) : path_(std::move(path)) {}

FileState BlackboxJsonFile::load() const {
    std::lock_guard lock(mutex_);
    FileState state;
    if (!fs::exists(path_)) return state;
    const auto size = fs::file_size(path_);
    if (size && *size > kBlackboxFileMaxBytes) {
        // TS は読めないファイルを空状態として扱い、次の save で上書きして消してしまう。
        // 上限超えは壊れたのではなく大きすぎるだけなので、消さないよう読み込みを拒否する。
        throw std::runtime_error("blackbox: state file exceeds size limit");
    }
    const auto text = fs::read_file(path_, kBlackboxFileMaxBytes);
    if (!text) return state;
    auto parsed = parse_json(*text, file_limits());
    if (!parsed || !parsed->is_object()) return state;  // 壊れた JSON は空状態 (throw しない)
    for (const auto& r : json_array(*parsed, "rules"))
        if (auto rule = rule_from_json(r)) state.rules.push_back(std::move(*rule));
    for (const auto& d : json_array(*parsed, "decisions"))
        if (auto rec = decision_record_from_json(d)) state.decisions.push_back(std::move(*rec));
    auto seq = parsed->find("seq");
    state.seq = seq != parsed->end() && seq->is_number() ? seq->get<std::int64_t>() : 0;
    return state;
}

void BlackboxJsonFile::save(const FileState& state) const {
    std::lock_guard lock(mutex_);
    Json rules = Json::array();
    for (const auto& r : state.rules) rules.push_back(to_json(r));
    Json decisions = Json::array();
    for (const auto& d : state.decisions) decisions.push_back(decision_record_to_file_json(d));
    const Json out = Json{{"rules", std::move(rules)}, {"decisions", std::move(decisions)}, {"seq", state.seq}};
    fs::ensure_dir(fs::dirname(path_));
    if (!fs::write_file_atomic(path_, json_stringify_pretty(out, 2) + "\n"))
        throw std::runtime_error("blackbox: failed to write state file");
}

// ── RuleStore ──────────────────────────────────────────────────────────────

JsonFileRuleStore::JsonFileRuleStore(std::shared_ptr<BlackboxJsonFile> file, const JsonFileStoreOptions& opts)
    : file_(std::move(file)), now_(or_now(opts.now)) {}

std::vector<Rule> JsonFileRuleStore::list_by_domain(const std::string& domain) {
    std::vector<Rule> out;
    for (auto& r : file_->load().rules)
        if (r.domain == domain) out.push_back(std::move(r));
    sort_rules(out);
    return out;
}

std::optional<Rule> JsonFileRuleStore::find_by_fingerprint(const std::string& domain, const std::string& fingerprint) {
    for (auto& r : file_->load().rules)
        if (r.domain == domain && r.fingerprint == fingerprint) return std::move(r);
    return std::nullopt;
}

std::optional<Rule> JsonFileRuleStore::get(const std::string& id) {
    for (auto& r : file_->load().rules)
        if (r.id == id) return std::move(r);
    return std::nullopt;
}

Rule JsonFileRuleStore::insert(const RuleDraft& draft) {
    std::lock_guard lock(file_->mutex());
    auto state = file_->load();
    Rule rule = make_rule_from_draft(draft, crypto::uuid_v4(), now_());
    state.rules.push_back(rule);
    file_->save(state);
    return rule;
}

std::optional<Rule> JsonFileRuleStore::update(const std::string& id, const RulePatch& patch) {
    std::lock_guard lock(file_->mutex());
    auto state = file_->load();
    for (auto& r : state.rules) {
        if (r.id != id) continue;
        apply_patch(r, patch);
        r.updatedAt = now_();
        Rule next = r;
        file_->save(state);
        return next;
    }
    return std::nullopt;
}

// ── DecisionLedger ─────────────────────────────────────────────────────────

JsonFileDecisionLedger::JsonFileDecisionLedger(std::shared_ptr<BlackboxJsonFile> file, const JsonFileStoreOptions& opts)
    : file_(std::move(file)), max_decisions_(opts.maxDecisions.value_or(1000)) {}

std::int64_t JsonFileDecisionLedger::record(const DecisionInput& rec) {
    std::lock_guard lock(file_->mutex());
    auto state = file_->load();
    const std::int64_t id = ++state.seq;
    state.decisions.push_back(make_record(rec, id));
    if (static_cast<std::int64_t>(state.decisions.size()) > max_decisions_) {
        // state.decisions.slice(-maxDecisions)
        const auto keep = static_cast<std::size_t>(std::max<std::int64_t>(0, max_decisions_));
        if (keep == 0) {
            // slice(-0) は全件。TS と同じく丸めない。
        } else {
            state.decisions.erase(state.decisions.begin(),
                                  state.decisions.end() - static_cast<std::ptrdiff_t>(keep));
        }
    }
    file_->save(state);
    return id;
}

std::optional<DecisionRecord> JsonFileDecisionLedger::get(std::int64_t id) {
    for (auto& d : file_->load().decisions)
        if (d.id == id) return std::move(d);
    return std::nullopt;
}

void JsonFileDecisionLedger::set_verdict(std::int64_t id, const std::string& verdict, const std::string& reviewed_at) {
    std::lock_guard lock(file_->mutex());
    auto state = file_->load();
    for (auto& d : state.decisions) {
        if (d.id != id) continue;
        d.verdict = verdict;
        d.reviewedAt = reviewed_at;
        file_->save(state);
        return;
    }
}

std::vector<DecisionRecord> JsonFileDecisionLedger::list_pending(const std::optional<std::string>& domain, std::int64_t limit) {
    std::vector<DecisionRecord> f;
    for (auto& d : file_->load().decisions)
        if (d.status == "pending_review" && !d.verdict && (!domain || domain->empty() || d.domain == *domain))
            f.push_back(std::move(d));
    return tail_reversed(std::move(f), limit);
}

std::vector<DecisionRecord> JsonFileDecisionLedger::list_recent(const std::string& domain, std::int64_t limit) {
    std::vector<DecisionRecord> f;
    for (auto& d : file_->load().decisions)
        if (d.domain == domain) f.push_back(std::move(d));
    return tail_reversed(std::move(f), limit);
}

BlackBox make_file_blackbox(const std::string& path, const EngineOptions& engine_opts, const JsonFileStoreOptions& store_opts) {
    auto file = std::make_shared<BlackboxJsonFile>(path);
    JsonFileStoreOptions o = store_opts;
    if (!o.now) o.now = engine_opts.now;
    BlackBox bb;
    bb.rules = std::make_shared<JsonFileRuleStore>(file, o);
    bb.ledger = std::make_shared<JsonFileDecisionLedger>(file, o);
    bb.engine = std::make_shared<BlackBoxEngine>(bb.rules, bb.ledger, engine_opts);
    return bb;
}

} // namespace di::lib::blackbox
