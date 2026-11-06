#pragma once
// Scenario/skill/placement contracts and explicit conditional play envelopes
// (src/level-check/contracts.ts の移植)。
#include <optional>
#include <string>
#include <vector>

#include "base/json.hpp"
#include "design_diagnostic/contracts.hpp"

namespace di::level_check {

using LevelCheckError = design_diagnostic::DiagnosticError;
using LevelReference = design_diagnostic::DiagnosticReference;

struct LevelCheckLimits : design_diagnostic::DiagnosticLimits {
    std::size_t events = 32;
    std::size_t scenarios = 16;
    std::size_t outcomesPerScenario = 128;
    std::size_t totalOutcomes = 256;
};
inline constexpr LevelCheckLimits LEVEL_CHECK_LIMITS{};

// Determinant: "random" | "player_skill" | "mixed" | "deterministic" | "unknown"
inline const std::vector<std::string>& determinants() {
    static const std::vector<std::string> v{"random", "player_skill", "mixed", "deterministic", "unknown"};
    return v;
}

struct PlayEvent {
    std::string id;
    std::string description;
    std::string determinant;
    std::vector<std::string> evidenceRefs;
};

struct PlayOutcome {
    std::string id;
    /** Exact joint occurrence; unlisted events are absent. No independence assumption. */
    std::vector<std::string> eventIds;
    double value = 0;
    std::string unit;
    std::optional<double> probability;
};

struct PlayAssumptions {
    std::optional<std::string> ruleset;
    std::optional<std::string> placement;
    std::optional<std::string> information;
    std::optional<std::string> initialState;
    std::optional<double> timeLimitSeconds;
    std::optional<double> trialCount;
    std::optional<std::vector<std::string>> other;
};

struct PredictedBaseline {
    double value = 0;
    std::string unit;
};

struct PlayScenario {
    std::string id;
    std::string description;
    std::optional<std::string> skillBand;
    PlayAssumptions assumptions;
    std::optional<PredictedBaseline> predictedBaseline;
    bool exhaustive = false;
    std::vector<PlayOutcome> outcomes;
};

struct PlayMetric {
    std::string id;
    std::string name;
    std::string unit;
    std::string direction;  // "higher" | "lower"
};

struct PlayModel {
    PlayMetric metric;
    std::vector<PlayEvent> events;
    std::vector<PlayScenario> scenarios;
};

struct LevelCheckRequest {
    std::string levelText;
    std::optional<std::string> baselineText;
    std::optional<std::vector<LevelReference>> references;
    std::optional<PlayModel> playModel;
};

inline const std::vector<std::string>& level_difference_kinds() {
    static const std::vector<std::string> v{"scenario_condition", "placement", "skill_condition", "predictability", "revision"};
    return v;
}
using LevelTextDifference = design_diagnostic::TextDifference;

struct PlayEnvelope {
    std::string scenarioId;
    PlayMetric metric;
    std::optional<std::string> skillBand;  // null は nullopt
    std::string determinant;
    std::string rangeScope;  // "enumerated_complete" | "enumerated_partial"
    double min = 0;
    double max = 0;
    std::optional<double> predictedBaseline;
    std::optional<double> upside;
    std::optional<double> downside;
    std::optional<double> expectedValue;
    std::optional<double> variance;
    std::string probabilityStatus;  // "complete" | "missing_or_partial" | "non_exhaustive"
};

struct PlayEnvelopeDifference : design_diagnostic::DiagnosticFinding {
    std::string kind = "play_envelope";
    /** Computed on the server, never accepted from the LLM. */
    PlayEnvelope envelope;
};

/** logic_differences の要素 (テキスト差分 か play_envelope)。 */
struct LevelLogicDifference {
    std::optional<LevelTextDifference> text;
    std::optional<PlayEnvelopeDifference> envelope;
};

struct LevelCheckResult {
    std::vector<LevelLogicDifference> logic_differences;
    std::vector<design_diagnostic::DesignGap> design_gap;
};

Json to_json(const PlayEvent& v);
Json to_json(const PlayOutcome& v);
Json to_json(const PlayScenario& v);
Json to_json(const PlayMetric& v);
Json to_json(const PlayModel& v);
Json to_json(const PlayEnvelope& v);
Json to_json(const PlayEnvelopeDifference& v);
Json to_json(const LevelLogicDifference& v);
Json to_json(const LevelCheckResult& v);

} // namespace di::level_check
