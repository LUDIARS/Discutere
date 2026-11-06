#include "level_check/play_envelope.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace di::level_check {

namespace {

std::string determinant(const PlayModel& model, const PlayScenario& scenario) {
    std::set<std::string> included;
    for (const auto& o : scenario.outcomes) included.insert(o.eventIds.begin(), o.eventIds.end());
    std::set<std::string> factors;
    for (const auto& e : model.events)
        if (included.count(e.id)) factors.insert(e.determinant);
    if (factors.count("unknown")) return "unknown";
    if (factors.count("mixed") || (factors.count("random") && factors.count("player_skill"))) return "mixed";
    if (factors.count("random")) return "random";
    if (factors.count("player_skill")) return "player_skill";
    return "deterministic";
}

double checked(double value) {
    if (!std::isfinite(value)) throw LevelCheckError("invalid_input", "playModel: numeric calculation overflow");
    return value;
}

} // namespace

std::vector<PlayEnvelope> calculate_play_envelopes(const PlayModel& model) {
    std::vector<PlayEnvelope> out;
    for (const auto& scenario : model.scenarios) {
        const std::optional<double> baseline = scenario.predictedBaseline ? std::optional<double>(scenario.predictedBaseline->value) : std::nullopt;
        const bool complete = scenario.exhaustive &&
                              std::all_of(scenario.outcomes.begin(), scenario.outcomes.end(), [](const PlayOutcome& o) { return o.probability.has_value(); });
        // A fully specified zero-probability combination is not reachable support.
        double min = std::numeric_limits<double>::infinity();
        double max = -std::numeric_limits<double>::infinity();
        for (const auto& o : scenario.outcomes) {
            if (complete && !(o.probability.value_or(0) > 0)) continue;
            min = std::min(min, o.value);
            max = std::max(max, o.value);
        }
        // Probability validation happened at the boundary. No inferred independent marginals.
        std::optional<double> expected;
        if (complete) {
            double sum = 0;
            for (const auto& o : scenario.outcomes) sum = checked(sum + checked(o.value * o.probability.value_or(0)));
            expected = checked(sum);
        }
        std::optional<double> variance;
        if (expected) {
            double sum = 0;
            for (const auto& o : scenario.outcomes) {
                if (o.probability && *o.probability == 0) continue;
                const double delta = checked(o.value - *expected);
                sum = checked(sum + checked(checked(delta * delta) * o.probability.value_or(0)));
            }
            variance = checked(sum);
        }
        const bool higher = model.metric.direction == "higher";
        PlayEnvelope e;
        e.scenarioId = scenario.id;
        e.metric = model.metric;
        e.skillBand = scenario.skillBand;
        e.determinant = determinant(model, scenario);
        e.rangeScope = scenario.exhaustive ? "enumerated_complete" : "enumerated_partial";
        e.min = min;
        e.max = max;
        e.predictedBaseline = baseline;
        if (baseline) {
            e.upside = std::max(0.0, checked(higher ? max - *baseline : *baseline - min));
            e.downside = std::max(0.0, checked(higher ? *baseline - min : max - *baseline));
        }
        e.expectedValue = expected;
        e.variance = variance;
        e.probabilityStatus = !scenario.exhaustive ? "non_exhaustive" : complete ? "complete" : "missing_or_partial";
        out.push_back(std::move(e));
    }
    return out;
}

} // namespace di::level_check
