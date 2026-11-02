#include "level_check/play_findings.hpp"

#include <algorithm>

namespace di::level_check {

namespace {

namespace dd = design_diagnostic;

bool truthy(const std::optional<std::string>& s) { return s && !s->empty(); }

dd::ComparedStatement model_statement(std::string text, std::string quote) {
    dd::ComparedStatement s;
    s.text = std::move(text);
    s.evidence.push_back({"playModel", std::move(quote)});
    return s;
}

std::vector<std::string> missing_conditions(const PlayScenario& scenario, const PlayEnvelope& envelope) {
    std::vector<std::string> missing;
    const auto& a = scenario.assumptions;
    if (!truthy(a.ruleset)) missing.push_back("適用ルールが未指定。配置差をルールの欠陥と断定できない。");
    if (!truthy(a.placement)) missing.push_back("配置・遭遇・出現条件が未指定。");
    if (!truthy(scenario.skillBand)) missing.push_back("技能帯が未指定。技能帯間の差をランダム分散と解釈できない。");
    if (!truthy(a.information)) missing.push_back("プレイヤーの情報条件が未指定。");
    if (!truthy(a.initialState)) missing.push_back("開始状態が未指定。");
    if (!a.timeLimitSeconds && !a.trialCount) missing.push_back("制限時間または試行回数が未指定。");
    if (!scenario.predictedBaseline) missing.push_back("本人の予測基準値が未指定。上振れ・下振れは unknown。");
    if (!scenario.exhaustive) missing.push_back("結果が非網羅。列挙した部分の到達幅であり、全体の限界は unknown。");
    if (std::any_of(scenario.outcomes.begin(), scenario.outcomes.end(), [](const PlayOutcome& o) { return !o.probability; }))
        missing.push_back("同時確率が未指定または部分指定。期待値と分散は unknown。");
    if (envelope.determinant == "unknown") missing.push_back("事象の決定要因が unknown。");
    return missing;
}

std::vector<std::string> scenario_conditions(const PlayScenario& scenario) {
    const auto& a = scenario.assumptions;
    std::vector<std::string> out{"入力模型に対する条件付き計算。実プレイや面白さの証明ではない。"};
    if (truthy(a.ruleset)) out.push_back("適用ルール: " + *a.ruleset);
    if (truthy(a.placement)) out.push_back("配置・遭遇・出現: " + *a.placement);
    if (truthy(scenario.skillBand)) out.push_back("技能帯: " + *scenario.skillBand);
    if (truthy(a.information)) out.push_back("情報: " + *a.information);
    if (truthy(a.initialState)) out.push_back("開始状態: " + *a.initialState);
    if (a.timeLimitSeconds) out.push_back("制限時間: " + js_number_to_string(*a.timeLimitSeconds) + "秒");
    if (a.trialCount) out.push_back("試行回数: " + js_number_to_string(*a.trialCount));
    if (a.other) out.insert(out.end(), a.other->begin(), a.other->end());
    return out;
}

} // namespace

PlayFindings compose_play_findings(const LevelCheckRequest& request, const std::vector<PlayEnvelope>& envelopes) {
    PlayFindings out;
    if (!request.playModel) return out;
    const auto& model = *request.playModel;
    for (std::size_t i = 0; i < envelopes.size() && i < model.scenarios.size(); ++i) {
        const auto& envelope = envelopes[i];
        const auto& scenario = model.scenarios[i];
        const auto conditions = scenario_conditions(scenario);
        const auto missing = missing_conditions(scenario, envelope);
        auto unknowns = missing;
        unknowns.push_back("入力模型の条件付き計算であり、実プレイでこの到達幅が生じるかは未確認。");
        dd::ComparedStatement left;
        if (scenario.predictedBaseline) {
            left = model_statement("本人の予測基準値: " + js_number_to_string(scenario.predictedBaseline->value) + " " + model.metric.unit,
                                   json_stringify(Json{{"value", scenario.predictedBaseline->value}, {"unit", scenario.predictedBaseline->unit}}));
        } else {
            left.text = "本人の予測基準値は未提示。";
        }
        const auto right = model_statement("列挙した同時結果の幅: " + js_number_to_string(envelope.min) + ".." + js_number_to_string(envelope.max) + " " +
                                               model.metric.unit + " (" + envelope.rangeScope + ")",
                                           "\"id\":" + json_stringify(Json(scenario.id)));
        PlayEnvelopeDifference finding;
        finding.kind = "play_envelope";
        finding.target = scenario.id;
        finding.left = left;
        finding.right = right;
        finding.status = "unknown";
        finding.causeDomain = "unknown";
        finding.conditions = conditions;
        finding.unknowns = unknowns;
        finding.envelope = envelope;
        // Calculation describes the supplied model; it does not judge a prediction as proved.
        if (!missing.empty()) {
            dd::DesignGap gap;
            gap.kind = "missing_condition";
            gap.target = scenario.id;
            gap.left = left;
            gap.right = right;
            gap.status = "unknown";
            gap.causeDomain = "unknown";
            gap.conditions = conditions;
            gap.unknowns = missing;
            out.gaps.push_back(std::move(gap));
        }
        out.differences.push_back(std::move(finding));
    }
    return out;
}

} // namespace di::level_check
