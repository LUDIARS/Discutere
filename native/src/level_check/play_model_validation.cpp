#include "level_check/play_model_validation.hpp"

#include <algorithm>
#include <cmath>

#include "base/strings.hpp"
#include "design_diagnostic/documents.hpp"
#include "design_diagnostic/validation.hpp"
#include "visualize/locale_compare.hpp"

namespace di::level_check {

namespace {

namespace dd = design_diagnostic;
const std::string C = "invalid_input";

const Json& at(const Json& object, const char* key) {
    static const Json kMissing;
    const Json* f = dd::field(object, key);
    return f ? *f : kMissing;
}
bool has(const Json& object, const char* key) { return dd::field(object, key) != nullptr; }

std::vector<std::string> ids(const Json& value, const std::string& path, std::size_t max, const std::set<std::string>& available,
                             std::size_t min = 0) {
    dd::array(value, path, max, C, min);
    std::vector<std::string> out;
    for (std::size_t i = 0; i < value.size(); ++i) out.push_back(dd::id(value[i], path + "[" + std::to_string(i) + "]", C));
    dd::unique(out, path, C);
    for (const auto& v : out)
        if (!available.count(v)) dd::fail(C, path, "unknown reference ID");
    return out;
}

PlayEvent event(const Json& value, std::size_t i, const std::set<std::string>& references) {
    const auto& L = LEVEL_CHECK_LIMITS;
    const std::string path = "playModel.events[" + std::to_string(i) + "]";
    dd::object(value, path, {"id", "description", "determinant", "evidenceRefs"}, C);
    PlayEvent e;
    e.id = dd::id(at(value, "id"), path + ".id", C);
    e.description = dd::string(at(value, "description"), path + ".description", L.shortTextChars, C);
    e.determinant = dd::choice(at(value, "determinant"), determinants(), path + ".determinant", C);
    e.evidenceRefs = ids(at(value, "evidenceRefs"), path + ".evidenceRefs", L.references + 2, references, 1);
    return e;
}

PlayOutcome outcome(const Json& value, const std::string& path, const std::set<std::string>& events, const std::string& unit) {
    const auto& L = LEVEL_CHECK_LIMITS;
    dd::object(value, path, {"id", "eventIds", "value", "unit", "probability"}, C);
    const std::string outcome_unit = dd::string(at(value, "unit"), path + ".unit", 80, C);
    if (outcome_unit != unit) dd::fail(C, path + ".unit", "does not match metric unit");
    std::optional<double> probability;
    if (has(value, "probability")) probability = dd::finite(at(value, "probability"), path + ".probability", C);
    if (probability && (*probability < 0 || *probability > 1)) dd::fail(C, path + ".probability", "outside 0..1");
    PlayOutcome o;
    o.id = dd::id(at(value, "id"), path + ".id", C);
    o.eventIds = ids(at(value, "eventIds"), path + ".eventIds", L.events, events);
    o.value = dd::finite(at(value, "value"), path + ".value", C);
    o.unit = outcome_unit;
    o.probability = probability;
    return o;
}

bool is_safe_integer(double n) { return std::isfinite(n) && std::trunc(n) == n && std::fabs(n) <= 9007199254740991.0; }

PlayScenario scenario(const Json& value, std::size_t i, const std::set<std::string>& events, const std::string& unit) {
    const auto& L = LEVEL_CHECK_LIMITS;
    const std::string path = "playModel.scenarios[" + std::to_string(i) + "]";
    dd::object(value, path, {"id", "description", "skillBand", "assumptions", "predictedBaseline", "exhaustive", "outcomes"}, C);
    const Json& a = at(value, "assumptions");
    dd::object(a, path + ".assumptions", {"ruleset", "placement", "information", "initialState", "timeLimitSeconds", "trialCount", "other"}, C);
    PlayScenario s;
    const auto text_field = [&](const char* key) -> std::optional<std::string> {
        if (!has(a, key)) return std::nullopt;
        return dd::string(at(a, key), path + ".assumptions." + key, L.shortTextChars - 32, C);
    };
    s.assumptions.ruleset = text_field("ruleset");
    s.assumptions.placement = text_field("placement");
    s.assumptions.information = text_field("information");
    s.assumptions.initialState = text_field("initialState");
    if (has(a, "timeLimitSeconds")) {
        const double n = dd::finite(at(a, "timeLimitSeconds"), path + ".assumptions.timeLimitSeconds", C);
        if (n <= 0) dd::fail(C, path, "time limit must be positive");
        s.assumptions.timeLimitSeconds = n;
    }
    if (has(a, "trialCount")) {
        const double n = dd::finite(at(a, "trialCount"), path + ".assumptions.trialCount", C);
        if (!is_safe_integer(n) || n <= 0) dd::fail(C, path, "trial count must be positive safe integer");
        s.assumptions.trialCount = n;
    }
    // Eight fixed condition descriptions may be added by the result composer.
    if (has(a, "other")) {
        const Json& other = at(a, "other");
        dd::array(other, path + ".assumptions.other", L.conditions - 8, C);
        std::vector<std::string> v;
        for (std::size_t j = 0; j < other.size(); ++j)
            v.push_back(dd::string(other[j], path + ".assumptions.other[" + std::to_string(j) + "]", L.shortTextChars, C));
        s.assumptions.other = std::move(v);
    }
    if (has(value, "predictedBaseline")) {
        const Json& b = at(value, "predictedBaseline");
        dd::object(b, path + ".predictedBaseline", {"value", "unit"}, C);
        const std::string baseline_unit = dd::string(at(b, "unit"), path + ".predictedBaseline.unit", 80, C);
        if (baseline_unit != unit) dd::fail(C, path, "baseline unit does not match metric");
        s.predictedBaseline = PredictedBaseline{dd::finite(at(b, "value"), path + ".predictedBaseline.value", C), baseline_unit};
    }
    if (!at(value, "exhaustive").is_boolean()) dd::fail(C, path + ".exhaustive", "expected boolean");
    const Json& outs = at(value, "outcomes");
    dd::array(outs, path + ".outcomes", L.outcomesPerScenario, C, 1);
    for (std::size_t j = 0; j < outs.size(); ++j) s.outcomes.push_back(outcome(outs[j], path + ".outcomes[" + std::to_string(j) + "]", events, unit));
    std::vector<std::string> outcome_ids, combos;
    for (const auto& o : s.outcomes) {
        outcome_ids.push_back(o.id);
        auto sorted = o.eventIds;
        std::stable_sort(sorted.begin(), sorted.end(), [](const std::string& x, const std::string& y) { return visualize::js_default_compare(x, y) < 0; });
        combos.push_back(str::join(sorted, std::string_view("\0", 1)));
    }
    dd::unique(outcome_ids, path + ".outcomes", C);
    dd::unique(combos, path + ".outcomes joint combinations", C);
    double total = 0;
    std::size_t with_probability = 0;
    for (const auto& o : s.outcomes) {
        if (!o.probability) continue;
        total += *o.probability;
        ++with_probability;
    }
    const bool exhaustive = at(value, "exhaustive").get<bool>();
    if (total > 1 + PROBABILITY_TOLERANCE) dd::fail(C, path, "supplied probabilities exceed 1");
    if (exhaustive && with_probability == s.outcomes.size() && std::fabs(total - 1) > PROBABILITY_TOLERANCE)
        dd::fail(C, path, "exhaustive joint probabilities must sum to 1");
    s.id = dd::id(at(value, "id"), path + ".id", C);
    s.description = dd::string(at(value, "description"), path + ".description", L.shortTextChars, C);
    if (has(value, "skillBand")) s.skillBand = dd::string(at(value, "skillBand"), path + ".skillBand", L.shortTextChars - 32, C);
    s.exhaustive = exhaustive;
    return s;
}

} // namespace

PlayModel validate_play_model(const Json& value, const std::set<std::string>& references) {
    const auto& L = LEVEL_CHECK_LIMITS;
    dd::object(value, "playModel", {"metric", "events", "scenarios"}, C);
    const Json& metric = at(value, "metric");
    dd::object(metric, "playModel.metric", {"id", "name", "unit", "direction"}, C);
    const std::string unit = dd::string(at(metric, "unit"), "playModel.metric.unit", 80, C);
    PlayModel model;
    const Json& events = at(value, "events");
    dd::array(events, "playModel.events", L.events, C);
    for (std::size_t i = 0; i < events.size(); ++i) model.events.push_back(event(events[i], i, references));
    std::vector<std::string> event_ids;
    for (const auto& e : model.events) event_ids.push_back(e.id);
    dd::unique(event_ids, "playModel.events", C);
    const std::set<std::string> event_set(event_ids.begin(), event_ids.end());
    const Json& scenarios = at(value, "scenarios");
    dd::array(scenarios, "playModel.scenarios", L.scenarios, C, 1);
    for (std::size_t i = 0; i < scenarios.size(); ++i) model.scenarios.push_back(scenario(scenarios[i], i, event_set, unit));
    std::vector<std::string> scenario_ids;
    std::size_t total_outcomes = 0;
    for (const auto& s : model.scenarios) {
        scenario_ids.push_back(s.id);
        total_outcomes += s.outcomes.size();
    }
    dd::unique(scenario_ids, "playModel.scenarios", C);
    if (total_outcomes > L.totalOutcomes) dd::fail(C, "playModel", "exceeds " + std::to_string(L.totalOutcomes) + " outcomes");
    model.metric.id = dd::id(at(metric, "id"), "playModel.metric.id", C);
    model.metric.name = dd::string(at(metric, "name"), "playModel.metric.name", L.shortTextChars, C);
    model.metric.unit = unit;
    model.metric.direction = dd::choice(at(metric, "direction"), {"higher", "lower"}, "playModel.metric.direction", C);
    return model;
}

} // namespace di::level_check
