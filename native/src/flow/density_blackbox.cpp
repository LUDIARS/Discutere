#include "flow/density_blackbox.hpp"

#include <cmath>
#include <memory>
#include <set>

#include "base/strings.hpp"

namespace di::flow {

namespace bb = lib::blackbox;

bb::FeatureMap density_features(const std::vector<ContextVoice>& voices, const std::vector<FlowTag>& tags) {
    std::set<std::string> kinds;
    double total_len = 0;
    for (const auto& v : voices) {
        kinds.insert(v.source);
        total_len += static_cast<double>(str::js_length(v.content));
    }
    // Math.round (0.5 は +∞ 方向)。
    const double avg = voices.empty() ? 0 : std::floor(total_len / static_cast<double>(voices.size()) + 0.5);
    return Json{{"voiceCount", static_cast<std::int64_t>(voices.size())},
                {"sourceKinds", static_cast<std::int64_t>(kinds.size())},
                {"avgLen", static_cast<std::int64_t>(avg)},
                {"tagCount", static_cast<std::int64_t>(tags.size())}};
}

std::optional<bb::RuleProposal> parse_proposed_rule(const std::optional<Json>& raw, const InformationDensity& density) {
    if (!raw || !(raw->is_object() || raw->is_array())) return std::nullopt;
    try {
        const Json when = raw->is_object() && raw->contains("when") ? (*raw)["when"] : Json();
        bb::RuleProposal p;
        p.when = bb::validate_condition(when);
        p.description = raw->is_object() && raw->contains("description") && (*raw)["description"].is_string()
                            ? (*raw)["description"].get<std::string>()
                            : "密度 " + density + " の閾値ルール";
        p.output = Json{{"density", density}};
        p.confidence = raw->is_object() && raw->contains("confidence") && (*raw)["confidence"].is_number()
                           ? (*raw)["confidence"].get<double>()
                           : 0.7;
        return p;
    } catch (...) {
        return std::nullopt;
    }
}

bb::BlackBox make_density_blackbox(const sql::DatabasePtr& db) { return bb::make_sqlite_blackbox(db); }

EvaluateFn make_blackbox_density_evaluator(bb::BlackBox box, EvaluateFn evaluate_fn) {
    if (!evaluate_fn) evaluate_fn = evaluate_information_density;
    // 取り込む値は共有ポインタで持つ (std::function に大きな値を直接入れると、静的解析が MSVC の
    // std::function の実装を読み違えて「一時オブジェクトの番地を返す」と誤検知するため)。
    struct Captured {
        bb::BlackBox box;
        EvaluateFn evaluate_fn;
    };
    auto captured = std::make_shared<const Captured>(Captured{std::move(box), std::move(evaluate_fn)});
    return [captured](const EvaluateArgs& args) -> InformationEvaluation {
        const auto features = density_features(args.voices, args.tags);
        std::optional<InformationEvaluation> llm_evaluation;

        const auto result = captured->box.engine->decide(DOMAIN_DENSITY, Json{{"theme", args.theme}}, features,
                                               [&](const Json&, const bb::FeatureMap&, const bb::LlmContext&) {
                                                   InformationEvaluation ev = captured->evaluate_fn(args);
                                                   llm_evaluation = ev;
                                                   bb::LlmJudgement j;
                                                   j.output = Json{{"density", ev.density}};
                                                   j.confidence = 0.7;
                                                   j.rationale = "covered " + std::to_string(ev.covered.size()) + " / gaps " +
                                                                 std::to_string(ev.gaps.size());
                                                   j.proposedRule = parse_proposed_rule(ev.proposedRule, ev.density);
                                                   return j;
                                               });

        // LLM 経路: gaps/covered 込みの完全な評価をそのまま返す。
        if (llm_evaluation) return *llm_evaluation;

        // ルール経路: 密度のみ。判定が不正な output を持っていたら安全側 (sparse=学習継続) に倒す。
        InformationEvaluation out;
        const Json& o = result.decision.output;
        out.density = o.is_object() && o.contains("density") && is_density(o["density"]) ? o["density"].get<std::string>() : "sparse";
        return out;
    };
}

} // namespace di::flow
