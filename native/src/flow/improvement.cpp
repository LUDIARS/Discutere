#include "flow/improvement.hpp"

#include "base/json.hpp"
#include "base/log.hpp"
#include "base/time.hpp"
#include "config.hpp"
#include "flow/cost_logger.hpp"
#include "flow/db/connection.hpp"
#include "flow/design_gap.hpp"
#include "flow/effect_predict.hpp"
#include "flow/sentiment_vector.hpp"

namespace di::flow {

void persist_improvement_scores(const std::string& session_id, int round, const std::vector<ScoredOpinion>& scored,
                                const std::optional<std::string>& winner) {
    auto db = db::get_flow_db();
    const auto now = time::now_ms();
    for (const auto& s : scored) {
        sql::Value detail;
        if (s.detail) {
            Json arr = Json::array();
            for (const auto& c : *s.detail) arr.push_back(to_json(c));
            detail = sql::Value(json_stringify(arr));
        }
        db->run("INSERT INTO improvement_score (session_id, round, utterance_id, score, is_winner, method, detail_json, created_at) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
                {session_id, round, s.id, s.score, winner && s.id == *winner ? 1 : 0, s.method, detail, now});
    }
}

FlowDirectorResult run_improvement_flow(persona_engine::LLMClient& llm, const std::string& theme, const std::vector<FlowTag>& tags,
                                        const ImprovementFlowDeps& deps) {
    const auto& cfg = config::get_config();
    const auto opinion_to_vector = deps.opinionToVector
                                       ? deps.opinionToVector
                                       : std::function<std::vector<double>(const std::string&)>(
                                             [](const std::string& t) { return lib::sentiment_core::text_to_vector(t); });
    const std::string games_dir = deps.gamesDir.value_or("./data/games");
    const WarnFn warn = deps.warn ? deps.warn : WarnFn([](const std::string& m) { di::log::warn("[flow/warn] " + m); });
    const std::string effect_model = cfg.flow.improvement.effectModel;

    // 現状 (起点) ベクトル: 明示指定 > sentiment.json (別名展開) > negative ベースライン。
    // ベースラインへの fallback は無言にしない (respec 07 §4)。
    std::optional<std::vector<double>> current = deps.currentVector;
    if (!current) {
        LoadCurrentVectorOpts lo;
        lo.warn = warn;
        current = load_current_vector(theme, games_dir, lo);
    }
    if (!current) {
        warn("テーマ「" + theme + "」の現状ベクトルが解決できないため negative ベースラインへフォールバックします");
        current = negative_baseline_vector();
    }
    const std::vector<double> resolved_current = *current;

    // 機械スコア評価器 (step 6 置換)。効果予測 LLM は with_cost_log 経由 (location="effect-predict")。
    FlowRunOptions o;
    static_cast<FlowDirectorDeps&>(o) = deps;
    o.flow = "improvement";
    o.evaluateRound = [resolved_current, opinion_to_vector, effect_model](const RoundEvalContext& ctx) {
        const auto target = build_target_vector(ctx.mechanics);
        const auto design_gap = compute_design_gap(resolved_current, target);
        CostLogContext c;
        c.flow = "improvement";
        c.sessionId = ctx.sessionId;
        c.round = ctx.round;
        c.role = "evaluator";
        c.location = "effect-predict";
        CostLoggedClient effect_llm(*ctx.llm, c);

        // 各意見の移動ベクトルを解決する。正 = LLM 効果予測 / 失敗時のみ旧 lexicon 方式 (意見単位)。
        std::vector<MovementCandidate> movements;
        for (const auto& cand : ctx.candidates) {
            PredictEffectArgs pa;
            pa.opinion = cand.text;
            pa.theme = ctx.theme;
            pa.mechanics = ctx.mechanics;
            if (!effect_model.empty()) pa.model = effect_model;
            pa.warn = ctx.warn;
            const auto prediction = predict_effect(effect_llm, pa);
            if (prediction) {
                movements.push_back({cand.id, changes_to_vector(prediction->changes), "effect-predict", prediction->changes});
            } else {
                if (ctx.warn)
                    ctx.warn("効果予測に失敗したため意見 " + cand.id + " (" + cand.personaName +
                             ") は旧 lexicon 方式で採点します (method=lexicon-fallback)");
                movements.push_back(
                    {cand.id, lib::sentiment_core::subtract(opinion_to_vector(cand.text), resolved_current), "lexicon-fallback", std::nullopt});
            }
        }

        const auto scored = score_opinions(movements, design_gap);
        const auto winner = pick_winner(scored);
        persist_improvement_scores(ctx.sessionId, ctx.round, scored, winner);

        // tally には機械スコアを載せる (conclusion は winner のみ参照)。
        // kind="scores" で票数でないことを明示し (respec 03, A10)、表示層が「n 票」表記にしない。
        // winnerShare (投票集中度) は票が無いため算出不能 = null (早期収束は従来条件のまま)。
        VoteResult r;
        r.kind = "scores";
        for (const auto& s : scored) {
            bool replaced = false;
            for (auto& [id, v] : r.tally)
                if (id == s.id) {
                    v = s.score;
                    replaced = true;
                }
            if (!replaced) r.tally.emplace_back(s.id, s.score);
        }
        r.winner = winner;
        return r;
    };
    return run_flow(llm, theme, tags, o);
}

} // namespace di::flow
