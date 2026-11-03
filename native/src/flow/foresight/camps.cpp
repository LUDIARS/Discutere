#include "flow/foresight/camps.hpp"

#include <algorithm>
#include <cmath>
#include <map>

#include "base/strings.hpp"

namespace di::flow::foresight {

namespace {

constexpr int TURNS_PER_EXCHANGE = 2;

std::string clip(const std::string& text) {
    const std::string t = str::trim(text);
    if (str::count_codepoints(t) > OWN_STATEMENT_CHARS) return str::prefix_codepoints(t, OWN_STATEMENT_CHARS) + "…";
    return t;
}

} // namespace

Json to_json(const ForesightSpeaker& s) {
    return Json{{"personaId", s.personaId},
                {"personaName", s.personaName},
                {"role", s.role},
                {"camp", s.camp},
                {"ownStatements", s.ownStatements}};
}

Json to_json(const ForesightCast& c) {
    Json speakers = Json::array();
    for (const auto& s : c.speakers) speakers.push_back(to_json(s));
    return Json{{"facilitator", Json{{"personaId", c.facilitator.personaId}, {"personaName", c.facilitator.personaName}, {"role", c.facilitator.role}}},
                {"speakers", std::move(speakers)}};
}

std::optional<ForesightCast> build_foresight_cast(const std::vector<FlowUtteranceRecord>& utterances) {
    ForesightCast cast;
    // 進行役が本議論に居ないときの代役。
    cast.facilitator = {"foresight-facilitator", "進行役", "facilitator"};
    bool has_facilitator = false;
    std::vector<const FlowUtteranceRecord*> order;
    std::map<std::string, std::vector<std::string>> statements;
    for (const auto& u : utterances) {
        if (u.isError) continue;
        if (u.role == "facilitator") {
            if (!has_facilitator) {
                cast.facilitator = {u.personaId, u.personaName, "facilitator"};
                has_facilitator = true;
            }
            continue;
        }
        auto it = statements.find(u.personaId);
        if (it == statements.end()) {
            order.push_back(&u);
            it = statements.emplace(u.personaId, std::vector<std::string>{}).first;
        }
        it->second.push_back(u.text);
    }
    if (order.size() < 2) return std::nullopt;

    for (std::size_t i = 0; i < order.size(); ++i) {
        const auto& u = *order[i];
        ForesightSpeaker s;
        s.personaId = u.personaId;
        s.personaName = u.personaName;
        s.role = u.role;
        s.camp = i % 2 == 0 ? "reform" : "conservative";
        const auto& all = statements[u.personaId];
        const std::size_t from = all.size() > OWN_STATEMENT_LIMIT ? all.size() - OWN_STATEMENT_LIMIT : 0;
        for (std::size_t k = from; k < all.size(); ++k) s.ownStatements.push_back(clip(all[k]));
        cast.speakers.push_back(std::move(s));
    }
    return cast;
}

int normalize_camp_turns(double turns) {
    double n = std::isfinite(turns) ? std::floor(turns) : TURNS_PER_EXCHANGE;
    n = std::min(n, static_cast<double>(MAX_CAMP_TURNS));
    const double rounded = std::ceil(n / TURNS_PER_EXCHANGE) * TURNS_PER_EXCHANGE;
    return static_cast<int>(std::max(static_cast<double>(TURNS_PER_EXCHANGE), rounded));
}

CampStage camp_stage_at(int turn_index) {
    static const std::vector<CampStage> stages = {"conditions", "refutation", "verification"};
    // JS の Math.floor (負の index は 0 段目より前 = undefined。ここでは先頭に寄せる)。
    const int exchange = turn_index < 0 ? 0 : turn_index / TURNS_PER_EXCHANGE;
    return stages[static_cast<std::size_t>(std::min(exchange, static_cast<int>(stages.size()) - 1))];
}

std::vector<ForesightSpeaker> build_camp_turn_order(const std::vector<ForesightSpeaker>& speakers, double turns) {
    std::vector<const ForesightSpeaker*> reform, conservative;
    for (const auto& s : speakers) {
        if (s.camp == "reform") reform.push_back(&s);
        if (s.camp == "conservative") conservative.push_back(&s);
    }
    if (reform.empty() || conservative.empty()) return {};
    std::vector<ForesightSpeaker> order;
    const int total = normalize_camp_turns(turns);
    for (int i = 0; i < total; ++i) {
        const auto& side = i % 2 == 0 ? reform : conservative;
        order.push_back(*side[static_cast<std::size_t>(i / 2) % side.size()]);
    }
    return order;
}

} // namespace di::flow::foresight
