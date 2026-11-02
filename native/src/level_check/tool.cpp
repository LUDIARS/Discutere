#include "level_check/tool.hpp"

#include <algorithm>

#include "base/strings.hpp"
#include "level_check/play_envelope.hpp"
#include "level_check/play_findings.hpp"
#include "level_check/prompt.hpp"
#include "level_check/request.hpp"
#include "level_check/response.hpp"

namespace di::level_check {

namespace L = design_diagnostic;

LevelCheckResult check_level_consistency(const Json& input, const LevelCheckDependencies& deps,
                                         std::optional<std::chrono::milliseconds> timeout) {
    return check_level_consistency(validate_level_check_request(input), deps, timeout);
}

LevelCheckResult check_level_consistency(const LevelCheckRequest& request, const LevelCheckDependencies& deps,
                                         std::optional<std::chrono::milliseconds> timeout) {
    const auto envelopes = request.playModel ? calculate_play_envelopes(*request.playModel) : std::vector<PlayEnvelope>{};
    const auto textual = parse_level_check_response(L::invoke_diagnostic(build_level_check_prompt(request), deps, timeout), request);
    const auto numeric = compose_play_findings(request, envelopes);

    LevelCheckResult result;
    for (const auto& d : textual.logic_differences) result.logic_differences.push_back(LevelLogicDifference{d, std::nullopt});
    for (const auto& d : numeric.differences) result.logic_differences.push_back(LevelLogicDifference{std::nullopt, d});
    result.design_gap = textual.design_gap;
    for (const auto& g : numeric.gaps) result.design_gap.push_back(g);

    const auto& refs = request.references;
    const bool observed = refs && std::any_of(refs->begin(), refs->end(), [](const auto& r) { return r.evidenceKind == "observation"; });
    if (!observed) {
        L::DesignGap gap;
        gap.kind = "missing_evidence";
        gap.target = "actual_level_experience";
        gap.left.text = "場面資料と条件付き模型の比較。";
        gap.left.evidence.push_back({"level", str::js_slice(str::trim(request.levelText), 0,
                                                             static_cast<std::int64_t>(LEVEL_CHECK_LIMITS.shortTextChars))});
        gap.right.text = "実プレイの観測資料は未提示。";
        gap.status = "unknown";
        gap.causeDomain = "unknown";
        gap.conditions = {"入力された場面・技能・配置・適用ルールの前提に限定。"};
        gap.unknowns = {"実プレイの到達幅や体験は未確認。数値幅だけでは原因の所在を特定できない。"};
        result.design_gap.push_back(std::move(gap));
    }
    if (json_stringify(to_json(result)).size() > LEVEL_CHECK_LIMITS.resultBytes)
        throw LevelCheckError("result_too_large", "Diagnostic result exceeds byte limit");
    return result;
}

} // namespace di::level_check
