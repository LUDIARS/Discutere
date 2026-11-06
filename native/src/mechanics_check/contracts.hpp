#pragma once
// Rule/resource/MDA diagnostic contracts; numeric scenario models belong to level-check
// (src/mechanics-check/contracts.ts の移植)。
#include <optional>
#include <string>
#include <vector>

#include "design_diagnostic/contracts.hpp"

namespace di::mechanics_check {

using MechanicsCheckError = design_diagnostic::DiagnosticError;
using MechanicsReference = design_diagnostic::DiagnosticReference;
using MechanicsFinding = design_diagnostic::DiagnosticFinding;
using EvidenceCitation = design_diagnostic::EvidenceCitation;
using ComparedStatement = design_diagnostic::ComparedStatement;
using DesignGap = design_diagnostic::DesignGap;

inline constexpr design_diagnostic::DiagnosticLimits MECHANICS_CHECK_LIMITS = design_diagnostic::DIAGNOSTIC_LIMITS;

inline const std::vector<std::string>& mechanics_difference_kinds() {
    static const std::vector<std::string> v{"mda_causality", "resource_flow", "rule_dependency", "revision"};
    return v;
}
using TextLogicDifference = design_diagnostic::TextDifference;

struct MechanicsCheckRequest {
    std::string specText;
    std::optional<std::string> baselineText;
    std::optional<std::vector<MechanicsReference>> references;
};

using MechanicsCheckResult = design_diagnostic::DiagnosticResult;

} // namespace di::mechanics_check
