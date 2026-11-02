#include "persona_engine/learning/metrics.hpp"

#include <algorithm>
#include <map>

#include "persona_engine/learning/learner.hpp"

namespace di::persona_engine::learning {

std::vector<PersonaMetric> persona_metrics(db::PersonasRepo& personas, db::RulesRepo& rules,
                                           const HypothesisStatusResolver& resolve_status, std::int64_t scan_limit) {
    const auto all = personas.list();
    struct Bucket {
        std::int64_t proposed = 0, integrated = 0, rejected = 0;
    };
    std::map<std::string, Bucket> by_persona;
    for (const auto& p : all) by_persona[p.id] = {};
    for (const auto& log : rules.recent_logs(scan_limit)) {
        if (log.action != "fire" || !log.detail || log.detail->empty()) continue;
        const auto m = match_propose_pattern(*log.detail);
        if (!m) continue;
        auto it = by_persona.find(m->personaId);
        if (it == by_persona.end()) continue;
        it->second.proposed += 1;
        const std::string status = resolve_status ? resolve_status(m->hypothesisId) : std::string("unknown");
        if (status == "integrated")
            it->second.integrated += 1;
        else if (status == "rejected")
            it->second.rejected += 1;
    }
    std::vector<PersonaMetric> out;
    out.reserve(all.size());
    for (const auto& p : all) {
        const Bucket& b = by_persona[p.id];
        PersonaMetric m{p.id, p.name, p.display_name, b.proposed, b.integrated, b.rejected, std::nullopt};
        if (b.proposed > 0) m.acceptanceRate = static_cast<double>(b.integrated) / static_cast<double>(b.proposed);
        out.push_back(std::move(m));
    }
    return out;
}

std::vector<RuleMetric> rule_metrics(db::RulesRepo& rules, std::int64_t scan_limit) {
    std::vector<RuleMetric> out;  // 初出順 (TS の Map の挿入順)
    std::map<std::string, std::size_t> index;
    for (const auto& log : rules.recent_logs(scan_limit)) {
        if (!log.rule_id || log.rule_id->empty()) continue;
        auto it = index.find(*log.rule_id);
        if (it == index.end()) {
            it = index.emplace(*log.rule_id, out.size()).first;
            out.push_back(RuleMetric{*log.rule_id, 0, 0, 0});
        }
        RuleMetric& m = out[it->second];
        if (log.action == "fire")
            m.fires += 1;
        else if (log.action == "skip")
            m.skips += 1;
        else if (log.action == "error")
            m.errors += 1;
    }
    // JS の Array#sort は安定
    std::stable_sort(out.begin(), out.end(), [](const RuleMetric& a, const RuleMetric& b) { return a.fires > b.fires; });
    return out;
}

Json to_json(const PersonaMetric& m) {
    Json j = Json::object();
    j["id"] = m.id;
    j["name"] = m.name;
    j["display_name"] = m.display_name;
    j["proposed"] = m.proposed;
    j["integrated"] = m.integrated;
    j["rejected"] = m.rejected;
    j["acceptanceRate"] = m.acceptanceRate ? Json(*m.acceptanceRate) : Json(nullptr);
    return j;
}

Json to_json(const RuleMetric& m) {
    Json j = Json::object();
    j["id"] = m.id;
    j["fires"] = m.fires;
    j["skips"] = m.skips;
    j["errors"] = m.errors;
    return j;
}

} // namespace di::persona_engine::learning
