#include "flow/argument_graph.hpp"

namespace di::flow {

DialogueAct coerce_act(const std::optional<std::string>& act) {
    static const char* const kActs[] = {"claim", "support", "rebut", "concede", "question", "synthesize"};
    if (act)
        for (const char* a : kActs)
            if (*act == a) return *act;
    return "claim";
}

ArgumentGraph::ClaimNode* ArgumentGraph::find_claim(const std::string& id) {
    auto it = claim_index_.find(id);
    return it == claim_index_.end() ? nullptr : &claims_[it->second];
}

RebuttalEdge* ArgumentGraph::find_rebuttal(const std::string& id) {
    auto it = rebuttal_index_.find(id);
    return it == rebuttal_index_.end() ? nullptr : &rebuttals_[it->second];
}

void ArgumentGraph::set_claim(ClaimNode node) {
    if (auto* existing = find_claim(node.id)) {
        *existing = std::move(node);
        return;
    }
    claim_index_[node.id] = claims_.size();
    claims_.push_back(std::move(node));
}

void ArgumentGraph::set_rebuttal(RebuttalEdge edge) {
    if (auto* existing = find_rebuttal(edge.rebutId)) {
        *existing = std::move(edge);
        return;
    }
    rebuttal_index_[edge.rebutId] = rebuttals_.size();
    rebuttals_.push_back(std::move(edge));
}

void ArgumentGraph::touch(const std::string& target_id) {
    if (auto* node = find_claim(target_id)) node->touched = true;
}

void ArgumentGraph::apply(const GraphUtterance& u) {
    const bool has_target = u.targetId && !u.targetId->empty();
    if (u.act == "claim" || u.act == "synthesize") {
        // synthesize も「新しい主張ノード」としては claim と同じ扱い (反論可能)。
        set_claim({u.id, u.personaId, u.turn, false});
        if (has_target) touch(*u.targetId);
        return;
    }
    if (u.act == "rebut") {
        if (!has_target) return;  // 対象なし rebut は論証上の効果なし (発話は残る)
        touch(*u.targetId);
        const ClaimNode* target = find_claim(*u.targetId);
        std::optional<std::string> target_persona;
        if (target) {
            target_persona = target->personaId;
        } else if (RebuttalEdge* target_edge = find_rebuttal(*u.targetId)) {
            // 反論への再反論 (論拠の応酬 dialectic.md §2.6): 被反論者が論拠で応答した = 元の反論は応答済み。
            // 新しい反論の応答義務は元の反論者が負う。
            if (target_edge->state == "challenged" && u.personaId != target_edge->byPersonaId)
                target_edge->state = "defended";
            target_persona = target_edge->byPersonaId;
        }
        set_rebuttal({u.id, *u.targetId, u.personaId, target_persona, "challenged"});
        return;
    }
    if (u.act == "support") {
        if (!has_target) return;
        if (RebuttalEdge* edge = find_rebuttal(*u.targetId)) {
            // 相手の rebut への応答 = 防御。反論者自身の support は防御にならない。
            if (edge->state == "challenged" && u.personaId != edge->byPersonaId) edge->state = "defended";
        } else {
            // claim への support = 補強 (touched のみ)。
            touch(*u.targetId);
        }
        return;
    }
    if (u.act == "concede") {
        if (!has_target) return;
        if (RebuttalEdge* edge = find_rebuttal(*u.targetId)) {
            if (edge->state == "challenged") {
                edge->state = "conceded";
                conceded_.push_back({u.id, edge->rebutId, edge->targetId});
            }
        } else {
            // claim を直接 concede した場合: その claim への未応答 rebut を全部 conceded にする。
            touch(*u.targetId);
            for (auto& e : rebuttals_) {
                if (e.targetId == *u.targetId && e.state == "challenged") {
                    e.state = "conceded";
                    conceded_.push_back({u.id, e.rebutId, e.targetId});
                }
            }
        }
        return;
    }
    if (u.act == "question") {
        if (has_target) touch(*u.targetId);
    }
}

std::vector<RebuttalEdge> ArgumentGraph::unresolved_rebuttals() const {
    std::vector<RebuttalEdge> out;
    for (const auto& e : rebuttals_)
        if (e.state == "challenged") out.push_back(e);
    return out;
}

std::vector<std::string> ArgumentGraph::unanswered_claims() const {
    std::vector<std::string> out;
    for (const auto& c : claims_)
        if (!c.touched) out.push_back(c.id);
    return out;
}

std::vector<ConcessionEdge> ArgumentGraph::concessions() const { return conceded_; }

int ArgumentGraph::new_claims_since(double turn) const {
    int n = 0;
    for (const auto& c : claims_)
        if (c.turn > turn) ++n;
    return n;
}

std::vector<ClaimNodeView> ArgumentGraph::claim_nodes() const {
    std::vector<ClaimNodeView> out;
    for (const auto& c : claims_) out.push_back({c.id, c.personaId, c.turn});
    return out;
}

int ArgumentGraph::edge_count_for(const std::string& claim_id) const {
    int n = 0;
    for (const auto& e : rebuttals_)
        if (e.targetId == claim_id) ++n;
    return n;
}

std::vector<RebuttalEdge> ArgumentGraph::rebuttals_targeting(const std::string& claim_id) const {
    std::vector<RebuttalEdge> out;
    for (const auto& e : rebuttals_)
        if (e.targetId == claim_id) out.push_back(e);
    return out;
}

std::optional<RebuttalEdge> ArgumentGraph::rebuttal_by_id(const std::string& rebut_id) const {
    auto it = rebuttal_index_.find(rebut_id);
    if (it == rebuttal_index_.end()) return std::nullopt;
    return rebuttals_[it->second];
}

ArgumentGraph build_graph_from_rows(const std::vector<UtteranceRow>& rows) {
    ArgumentGraph g;
    for (const auto& r : rows) g.apply({r.id, r.persona_id, coerce_act(r.act), r.target_id, r.turn});
    return g;
}

} // namespace di::flow
