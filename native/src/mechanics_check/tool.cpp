#include "mechanics_check/tool.hpp"

#include <algorithm>

#include "base/strings.hpp"
#include "mechanics_check/prompt.hpp"
#include "mechanics_check/request.hpp"
#include "mechanics_check/response.hpp"

namespace di::mechanics_check {

namespace L = design_diagnostic;

MechanicsCheckResult check_mechanics_consistency(const Json& input, const MechanicsCheckDependencies& deps,
                                                 std::optional<std::chrono::milliseconds> timeout) {
    return check_mechanics_consistency(validate_mechanics_check_request(input), deps, timeout);
}

MechanicsCheckResult check_mechanics_consistency(const MechanicsCheckRequest& request, const MechanicsCheckDependencies& deps,
                                                 std::optional<std::chrono::milliseconds> timeout) {
    auto result = parse_mechanics_check_response(L::invoke_diagnostic(build_mechanics_check_prompt(request), deps, timeout), request);
    const auto& refs = request.references;
    const bool observed = refs && std::any_of(refs->begin(), refs->end(), [](const auto& r) { return r.evidenceKind == "observation"; });
    if (!observed) {
        DesignGap gap;
        gap.kind = "missing_evidence";
        gap.target = "actual_play_experience";
        gap.left.text = "入力仕様に対する意図・因果の比較。";
        gap.left.evidence.push_back({"spec", str::js_slice(str::trim(request.specText), 0,
                                                            static_cast<std::int64_t>(MECHANICS_CHECK_LIMITS.shortTextChars))});
        gap.right.text = "実プレイの観測資料は未提示。";
        gap.status = "unknown";
        gap.causeDomain = "unknown";
        gap.conditions = {"ルール・資源・MDAの条件付き論理比較。"};
        gap.unknowns = {"実際の体験や面白さの証明は unknown。模型仮定・原因仮説を観測に置き換えない。"};
        result.design_gap.push_back(std::move(gap));
    }
    if (json_stringify(L::to_json(result)).size() > MECHANICS_CHECK_LIMITS.resultBytes)
        throw MechanicsCheckError("result_too_large", "Diagnostic result exceeds byte limit");
    return result;
}

} // namespace di::mechanics_check
