#include "lib/blackbox/engine.hpp"

#include <algorithm>

#include "base/json.hpp"
#include "base/time.hpp"
#include "lib/blackbox/condition.hpp"
#include "lib/blackbox/fingerprint.hpp"

namespace di::lib::blackbox {

BlackBoxEngine::BlackBoxEngine(std::shared_ptr<RuleStore> rules, std::shared_ptr<DecisionLedger> ledger, const EngineOptions& opts)
    : rules_(std::move(rules)), ledger_(std::move(ledger)) {
    t_.autoPromote = opts.autoPromote.value_or(DEFAULT_THRESHOLDS.autoPromote);
    t_.autoRetract = opts.autoRetract.value_or(DEFAULT_THRESHOLDS.autoRetract);
    t_.shadowPromote = opts.shadowPromote.value_or(DEFAULT_THRESHOLDS.shadowPromote);
    t_.shadowConflictLimit = opts.shadowConflictLimit.value_or(DEFAULT_THRESHOLDS.shadowConflictLimit);
    review_llm_ = opts.reviewLlmDecisions.value_or(true);
    now_ = opts.now ? opts.now : [] { return time::now_iso(); };
}

std::optional<Rule> BlackBoxEngine::first_live_match(const std::vector<Rule>& rules, const FeatureMap& features) const {
    std::vector<const Rule*> live;
    for (const auto& r : rules)
        if (is_live(r.state)) live.push_back(&r);
    std::stable_sort(live.begin(), live.end(), [](const Rule* a, const Rule* b) { return b->priority < a->priority; });
    for (const Rule* r : live)
        if (evaluate(r->when, features)) return *r;
    return std::nullopt;
}

DecideResult BlackBoxEngine::decide(const std::string& domain, const Json& input, const FeatureMap& features,
                                    const LlmFallback& llm_fallback) {
    const auto all = rules_->list_by_domain(domain);
    const auto hit = first_live_match(all, features);

    if (hit) {
        const bool trusted = hit->state == "auto";
        // 教師にできるのは卒業済み (auto) ルールの出力のみ。trial の出力はまだ検証中。
        auto shadow = trusted ? run_shadow(all, features, hit->output) : std::vector<ShadowEval>{};
        Decision decision;
        decision.output = hit->output;
        decision.source = "rule";
        decision.ruleId = hit->id;
        decision.confidence = hit->confidence;
        decision.rationale = trusted ? "ルール「" + hit->description + "」で判定 (" + describe_condition(hit->when) + ")"
                                     : "ルール「" + hit->description + "」で先に判定しました — OK/NG をお願いします (承認 " +
                                           std::to_string(hit->approvals) + "/" + std::to_string(t_.autoPromote) + ")";
        decision.status = trusted ? "auto" : "pending_review";
        DecisionInput rec{domain, input, features, decision.output, "rule", hit->id, decision.confidence,
                          decision.rationale, decision.status, std::move(shadow), now_()};
        const auto id = ledger_->record(rec);
        return {decision, id};
    }

    // ルール無し → LLM に判断させる。撤回済みルールを知らせて再提案を防ぐ。
    LlmContext ctx;
    ctx.domain = domain;
    for (const auto& r : all)
        if (r.state == "retired") ctx.retiredRules.push_back({r.description, describe_condition(r.when)});
    const LlmJudgement j = llm_fallback(input, features, ctx);

    const auto proposed_id = j.proposedRule ? propose(domain, *j.proposedRule) : std::nullopt;
    // 影評価: 既存 candidate を LLM の出力 (教師) と突合。いま提案されたばかりのルールは除外する。
    const auto fresh = proposed_id ? rules_->list_by_domain(domain) : all;
    auto shadow = run_shadow(fresh, features, j.output, proposed_id);

    Decision decision;
    decision.output = j.output;
    decision.source = "llm";
    decision.confidence = j.confidence;
    decision.rationale = "LLM が判定: " + j.rationale;
    decision.status = review_llm_ ? "pending_review" : "auto";
    DecisionInput rec{domain, input, features, decision.output, "llm", std::nullopt, decision.confidence,
                      decision.rationale, decision.status, std::move(shadow), now_()};
    const auto id = ledger_->record(rec);
    return {decision, id};
}

std::optional<std::string> BlackBoxEngine::propose(const std::string& domain, const RuleProposal& proposal) {
    const std::string fingerprint = rule_fingerprint(proposal.when, proposal.output);
    const auto existing = rules_->find_by_fingerprint(domain, fingerprint);
    if (existing) {
        if (existing->state == "retired") return std::nullopt;
        RulePatch p;
        p.proposals = existing->proposals + 1;
        p.confidence = std::max(existing->confidence, proposal.confidence.value_or(0));
        rules_->update(existing->id, p);
        return std::nullopt;
    }
    try {
        RuleDraft draft;
        draft.domain = domain;
        draft.description = proposal.description;
        draft.when = proposal.when;
        draft.output = proposal.output;
        draft.confidence = proposal.confidence;
        draft.priority = proposal.priority;
        draft.source = "llm";
        draft.state = "candidate";
        return rules_->insert(draft).id;
    } catch (const std::exception&) {
        return std::nullopt;  // 候補登録の失敗は判断本体を止めない
    }
}

std::vector<ShadowEval> BlackBoxEngine::run_shadow(const std::vector<Rule>& rules, const FeatureMap& features,
                                                   const Json& teacher_output, const std::optional<std::string>& exclude_id) {
    std::vector<ShadowEval> evals;
    for (const auto& r : rules) {
        if (r.state != "candidate" || (exclude_id && r.id == *exclude_id)) continue;
        if (!evaluate(r.when, features)) continue;
        const bool agreed = same_output(r.output, teacher_output);
        evals.push_back({r.id, agreed});
        const auto patch = shadow_patch(r, agreed ? ShadowDelta{1, 0} : ShadowDelta{0, 1}, t_);
        if (patch) rules_->update(r.id, *patch);
    }
    return evals;
}

VerdictResult BlackBoxEngine::record_verdict(std::int64_t decision_id, const std::string& verdict) {
    const auto rec = ledger_->get(decision_id);
    if (!rec) return {};
    ledger_->set_verdict(decision_id, verdict, now_());

    if (rec->source == "rule" && rec->ruleId) {
        const auto rule = rules_->get(*rec->ruleId);
        if (!rule) return {true, std::nullopt};
        return {true, rules_->update(rule->id, verdict_patch(*rule, verdict, t_))};
    }

    if (rec->source == "llm" && verdict == "ng") {
        for (const auto& ev : rec->shadow) {
            const auto rule = rules_->get(ev.ruleId);
            if (!rule || rule->state != "candidate") continue;
            // 教師 (LLM) が NG → 一致は誤った加点だったので衝突へ振り替え。
            // 不一致は「実は正しかったかもしれない」ので減点だけ取り消す (加点はしない)。
            const auto patch = shadow_patch(*rule, ev.agreed ? ShadowDelta{-1, 1} : ShadowDelta{0, -1}, t_);
            if (patch) rules_->update(ev.ruleId, *patch);
        }
    }
    return {true, std::nullopt};
}

Rule BlackBoxEngine::add_rule(const RuleDraft& draft) {
    RuleDraft d = draft;
    // draft.state が無ければ seed→auto / それ以外→trial。
    d.state = draft.state ? *draft.state : RuleState(draft.source && *draft.source == "seed" ? "auto" : "trial");
    return rules_->insert(d);
}

std::optional<Rule> BlackBoxEngine::set_rule_state(const std::string& id, const RuleState& state) {
    RulePatch p;
    p.state = state;
    return rules_->update(id, p);
}

std::vector<Rule> BlackBoxEngine::list_rules(const std::string& domain) { return rules_->list_by_domain(domain); }

} // namespace di::lib::blackbox
