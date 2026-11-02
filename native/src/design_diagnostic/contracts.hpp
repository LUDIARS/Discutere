#pragma once
// Shared documents and structured comparisons, without feature-specific computation
// (src/design-diagnostic/contracts.ts の移植)。
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.hpp"

namespace di::design_diagnostic {

struct DiagnosticLimits {
    std::size_t documentChars = 20'000;
    std::size_t referenceChars = 8'000;
    std::size_t totalTextChars = 64'000;
    std::size_t references = 16;
    std::size_t findingsPerKind = 32;
    std::size_t shortTextChars = 512;
    std::size_t statementChars = 2'048;
    std::size_t conditions = 16;
    std::size_t responseBytes = 131'072;
    std::size_t resultBytes = 524'288;
    std::size_t discordInputChars = 6'000;
    long long timeoutMs = 60'000;
};
inline constexpr DiagnosticLimits DIAGNOSTIC_LIMITS{};

// EvidenceKind: "specification" | "static_implementation" | "observation" | "model_assumption" | "causal_hypothesis"
inline const std::vector<std::string>& evidence_kinds() {
    static const std::vector<std::string> v{"specification", "static_implementation", "observation", "model_assumption", "causal_hypothesis"};
    return v;
}
// CauseDomain: "mechanics" | "level" | "both" | "unknown"
inline const std::vector<std::string>& cause_domains() {
    static const std::vector<std::string> v{"mechanics", "level", "both", "unknown"};
    return v;
}
// ConsistencyStatus: "consistent" | "inconsistent" | "unknown"
inline const std::vector<std::string>& consistency_statuses() {
    static const std::vector<std::string> v{"consistent", "inconsistent", "unknown"};
    return v;
}

struct DiagnosticReference {
    std::string id;
    std::string source;
    std::optional<std::string> revision;
    std::string text;
    std::string evidenceKind;
    /** Supplied content relevance, not proof of a causal attribution. Omitted = unknown. ("mechanics" | "level") */
    std::optional<std::string> domain;
};

struct EvidenceCitation {
    std::string referenceId;
    std::string quote;
};

struct ComparedStatement {
    std::string text;
    std::vector<EvidenceCitation> evidence;
};

struct DiagnosticFinding {
    std::string target;
    ComparedStatement left;
    ComparedStatement right;
    std::string status;
    /** Conditional cause hypothesis, never inferred merely from a numeric range. */
    std::string causeDomain;
    std::vector<std::string> conditions;
    std::vector<std::string> unknowns;
};

/** TextDifference<K> / DesignGap (kind つきの所見)。 */
struct KindedFinding : DiagnosticFinding {
    std::string kind;
};
using TextDifference = KindedFinding;
/** DesignGap: kind="missing_evidence"|"missing_condition" (status=unknown) / "contradiction" (status=inconsistent)。 */
using DesignGap = KindedFinding;

struct DiagnosticResult {
    std::vector<TextDifference> logic_differences;
    std::vector<DesignGap> design_gap;
};

Json to_json(const DiagnosticReference& v);
Json to_json(const EvidenceCitation& v);
Json to_json(const ComparedStatement& v);
/** {kind?, target, left, right, status, causeDomain, conditions, unknowns} (kind は先頭)。 */
Json to_json(const DiagnosticFinding& v);
Json to_json(const KindedFinding& v);
Json to_json(const DiagnosticResult& v);

// DiagnosticErrorCode
// "invalid_input" | "use_level_check" | "llm_unavailable" | "llm_failed" | "llm_timeout" | "invalid_response" | "result_too_large"
class DiagnosticError : public std::runtime_error {
public:
    DiagnosticError(std::string code, const std::string& message) : std::runtime_error(message), code_(std::move(code)) {}
    const std::string& code() const { return code_; }

private:
    std::string code_;
};

} // namespace di::design_diagnostic
