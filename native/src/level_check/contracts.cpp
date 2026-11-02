#include "level_check/contracts.hpp"

namespace di::level_check {

namespace {
Json opt(const std::optional<double>& v) { return v ? Json(*v) : Json(nullptr); }
} // namespace

Json to_json(const PlayEvent& v) {
    return Json{{"id", v.id}, {"description", v.description}, {"determinant", v.determinant}, {"evidenceRefs", v.evidenceRefs}};
}

Json to_json(const PlayOutcome& v) {
    Json o{{"id", v.id}, {"eventIds", v.eventIds}, {"value", v.value}, {"unit", v.unit}};
    if (v.probability) o["probability"] = *v.probability;
    return o;
}

Json to_json(const PlayScenario& v) {
    Json o = Json::object();
    o["id"] = v.id;
    o["description"] = v.description;
    if (v.skillBand) o["skillBand"] = *v.skillBand;
    Json a = Json::object();
    if (v.assumptions.ruleset) a["ruleset"] = *v.assumptions.ruleset;
    if (v.assumptions.placement) a["placement"] = *v.assumptions.placement;
    if (v.assumptions.information) a["information"] = *v.assumptions.information;
    if (v.assumptions.initialState) a["initialState"] = *v.assumptions.initialState;
    if (v.assumptions.timeLimitSeconds) a["timeLimitSeconds"] = *v.assumptions.timeLimitSeconds;
    if (v.assumptions.trialCount) a["trialCount"] = *v.assumptions.trialCount;
    if (v.assumptions.other) a["other"] = *v.assumptions.other;
    o["assumptions"] = a;
    if (v.predictedBaseline) o["predictedBaseline"] = Json{{"value", v.predictedBaseline->value}, {"unit", v.predictedBaseline->unit}};
    o["exhaustive"] = v.exhaustive;
    o["outcomes"] = Json::array();
    for (const auto& x : v.outcomes) o["outcomes"].push_back(to_json(x));
    return o;
}

Json to_json(const PlayMetric& v) { return Json{{"id", v.id}, {"name", v.name}, {"unit", v.unit}, {"direction", v.direction}}; }

Json to_json(const PlayModel& v) {
    Json o{{"metric", to_json(v.metric)}, {"events", Json::array()}, {"scenarios", Json::array()}};
    for (const auto& e : v.events) o["events"].push_back(to_json(e));
    for (const auto& s : v.scenarios) o["scenarios"].push_back(to_json(s));
    return o;
}

Json to_json(const PlayEnvelope& v) {
    Json o = Json::object();
    o["scenarioId"] = v.scenarioId;
    o["metric"] = to_json(v.metric);
    o["skillBand"] = v.skillBand ? Json(*v.skillBand) : Json(nullptr);
    o["determinant"] = v.determinant;
    o["rangeScope"] = v.rangeScope;
    o["min"] = v.min;
    o["max"] = v.max;
    o["predictedBaseline"] = opt(v.predictedBaseline);
    o["upside"] = opt(v.upside);
    o["downside"] = opt(v.downside);
    o["expectedValue"] = opt(v.expectedValue);
    o["variance"] = opt(v.variance);
    o["probabilityStatus"] = v.probabilityStatus;
    return o;
}

Json to_json(const PlayEnvelopeDifference& v) {
    Json o = Json::object();
    o["kind"] = v.kind;
    for (const auto& [k, val] : design_diagnostic::to_json(static_cast<const design_diagnostic::DiagnosticFinding&>(v)).items()) o[k] = val;
    o["envelope"] = to_json(v.envelope);
    return o;
}

Json to_json(const LevelLogicDifference& v) {
    if (v.envelope) return to_json(*v.envelope);
    if (v.text) return design_diagnostic::to_json(*v.text);
    return Json::object();
}

Json to_json(const LevelCheckResult& v) {
    Json logic = Json::array();
    for (const auto& f : v.logic_differences) logic.push_back(to_json(f));
    Json gaps = Json::array();
    for (const auto& g : v.design_gap) gaps.push_back(design_diagnostic::to_json(g));
    return Json{{"logic_differences", logic}, {"design_gap", gaps}};
}

} // namespace di::level_check
