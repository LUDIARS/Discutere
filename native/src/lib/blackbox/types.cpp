#include "lib/blackbox/types.hpp"

namespace di::lib::blackbox {

namespace {
Json opt_str(const std::optional<std::string>& v) { return v ? Json(*v) : Json(nullptr); }
std::optional<std::string> str_or_null(const Json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return std::nullopt;
    return it->get<std::string>();
}
Json member_or_null(const Json& j, const char* key) {
    auto it = j.find(key);
    return it == j.end() ? Json(nullptr) : *it;
}
} // namespace

Json to_json(const Rule& r) {
    return Json{{"id", r.id},
                {"domain", r.domain},
                {"description", r.description},
                {"when", r.when},
                {"output", r.output},
                {"confidence", r.confidence},
                {"state", r.state},
                {"source", r.source},
                {"approvals", r.approvals},
                {"rejections", r.rejections},
                {"shadowAgreements", r.shadowAgreements},
                {"shadowConflicts", r.shadowConflicts},
                {"proposals", r.proposals},
                {"fingerprint", r.fingerprint},
                {"priority", r.priority},
                {"createdAt", r.createdAt},
                {"updatedAt", r.updatedAt}};
}

Json to_json(const ShadowEval& e) { return Json{{"ruleId", e.ruleId}, {"agreed", e.agreed}}; }

Json to_json(const DecisionRecord& r) {
    Json shadow = Json::array();
    for (const auto& e : r.shadow) shadow.push_back(to_json(e));
    // store-sqlite の rowToDecision と同じ順。
    return Json{{"id", r.id},
                {"domain", r.domain},
                {"input", r.input},
                {"features", r.features},
                {"output", r.output},
                {"source", r.source},
                {"ruleId", opt_str(r.ruleId)},
                {"confidence", r.confidence},
                {"rationale", r.rationale},
                {"status", r.status},
                {"verdict", opt_str(r.verdict)},
                {"shadow", std::move(shadow)},
                {"createdAt", r.createdAt},
                {"reviewedAt", opt_str(r.reviewedAt)}};
}

Json to_json(const Decision& d) {
    Json j = Json{{"output", d.output}, {"source", d.source}};
    if (d.ruleId) j["ruleId"] = *d.ruleId;
    j["confidence"] = d.confidence;
    j["rationale"] = d.rationale;
    j["status"] = d.status;
    return j;
}

std::optional<Rule> rule_from_json(const Json& j) {
    if (!j.is_object()) return std::nullopt;
    auto id = str_or_null(j, "id");
    if (!id) return std::nullopt;
    Rule r;
    r.id = *id;
    r.domain = json_string(j, "domain");
    r.description = json_string(j, "description");
    r.when = member_or_null(j, "when");
    r.output = member_or_null(j, "output");
    r.confidence = json_number(j, "confidence", 0.7);
    r.state = json_string(j, "state", "candidate");
    r.source = json_string(j, "source", "manual");
    r.approvals = json_int(j, "approvals");
    r.rejections = json_int(j, "rejections");
    r.shadowAgreements = json_int(j, "shadowAgreements");
    r.shadowConflicts = json_int(j, "shadowConflicts");
    r.proposals = json_int(j, "proposals", 1);
    r.fingerprint = json_string(j, "fingerprint");
    r.priority = json_number(j, "priority");
    r.createdAt = json_string(j, "createdAt");
    r.updatedAt = json_string(j, "updatedAt");
    return r;
}

std::vector<ShadowEval> shadow_from_json(const Json& j) {
    std::vector<ShadowEval> out;
    if (!j.is_array()) return out;
    for (const auto& e : j) {
        if (!e.is_object()) continue;
        out.push_back({json_string(e, "ruleId"), json_bool(e, "agreed")});
    }
    return out;
}

std::optional<DecisionRecord> decision_record_from_json(const Json& j) {
    if (!j.is_object()) return std::nullopt;
    auto id = j.find("id");
    if (id == j.end() || !id->is_number()) return std::nullopt;
    DecisionRecord r;
    r.id = id->get<std::int64_t>();
    r.domain = json_string(j, "domain");
    r.input = member_or_null(j, "input");
    r.features = j.contains("features") && j.at("features").is_object() ? j.at("features") : Json::object();
    r.output = member_or_null(j, "output");
    r.source = json_string(j, "source");
    r.ruleId = str_or_null(j, "ruleId");
    r.confidence = json_number(j, "confidence");
    r.rationale = json_string(j, "rationale");
    r.status = json_string(j, "status");
    r.verdict = str_or_null(j, "verdict");
    r.shadow = shadow_from_json(member_or_null(j, "shadow"));
    r.createdAt = json_string(j, "createdAt");
    r.reviewedAt = str_or_null(j, "reviewedAt");
    return r;
}

} // namespace di::lib::blackbox
