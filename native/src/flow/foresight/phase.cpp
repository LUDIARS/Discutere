#include "flow/foresight/phase.hpp"

#include <algorithm>
#include <stdexcept>

#include "base/crypto.hpp"
#include "base/strings.hpp"
#include "flow/cost_logger.hpp"
#include "flow/foresight/camps.hpp"
#include "flow/foresight/markdown.hpp"

namespace di::flow::foresight {

std::vector<std::string> split_for_post(const std::string& text, std::size_t limit) {
    if (limit == 0) limit = 1;
    std::vector<std::string> chunks;
    std::string current;
    std::size_t current_len = 0;
    std::size_t start = 0;
    while (true) {
        const std::size_t nl = text.find('\n', start);
        const std::string line = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        const std::size_t line_len = str::count_codepoints(line);
        if (line_len > limit) {
            if (!current.empty()) chunks.push_back(current);
            current.clear();
            current_len = 0;
            const auto cps = str::codepoints(line);
            for (std::size_t i = 0; i < cps.size(); i += limit) {
                const std::size_t end = std::min(cps.size(), i + limit);
                chunks.push_back(str::from_codepoints(std::vector<std::uint32_t>(cps.begin() + static_cast<std::ptrdiff_t>(i),
                                                                                 cps.begin() + static_cast<std::ptrdiff_t>(end))));
            }
        } else {
            const std::size_t next_len = current.empty() ? line_len : current_len + 1 + line_len;
            if (next_len > limit) {
                chunks.push_back(current);
                current = line;
                current_len = line_len;
            } else {
                current = current.empty() ? line : current + "\n" + line;
                current_len = next_len;
            }
        }
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    if (!current.empty()) chunks.push_back(current);
    return chunks;
}

std::vector<std::string> label_parts(const std::vector<std::string>& chunks) {
    if (chunks.size() <= 1) return chunks;
    std::vector<std::string> out;
    for (std::size_t i = 0; i < chunks.size(); ++i)
        out.push_back(chunks[i] + "\n(" + std::to_string(i + 1) + "/" + std::to_string(chunks.size()) + ")");
    return out;
}

std::optional<ForesightPhaseResult> run_foresight_phase(persona_engine::LLMClient& llm, const ForesightPhaseArgs& args) {
    if (!args.commit) throw std::invalid_argument("run_foresight_phase: commit is required");
    const WarnFn log = args.log ? args.log : WarnFn([](const std::string&) {});
    const WarnFn warn = args.warn ? args.warn : WarnFn([](const std::string&) {});
    const auto cast = build_foresight_cast(args.utterances);
    if (!cast) {
        log("未来予測: 改革派と保守派に分けられる話者がいないため行わない");
        return std::nullopt;
    }

    int max_round = 0;
    for (const auto& u : args.utterances) max_round = std::max(max_round, u.round);
    const int round = max_round + 1;
    ForesightPhaseResult result;
    int turn = 0;

    auto post = [&](const std::string& persona_id, const std::string& persona_name, const std::string& role, const std::string& stance,
                    const std::string& text, bool is_error) {
        for (const auto& chunk : label_parts(split_for_post(text))) {
            FlowUtteranceRecord record;
            record.id = crypto::uuid_v4();
            record.sessionId = args.sessionId;
            record.paperId = args.paperId;
            record.round = round;
            record.turn = turn++;
            record.personaId = persona_id;
            record.personaName = persona_name;
            record.role = role;
            record.stance = stance;
            record.text = chunk;
            record.isError = is_error;
            result.utterances.push_back(record);
            args.commit(record);
        }
    };
    const auto& fac = cast->facilitator;
    auto post_fac = [&](const std::string& text, bool is_error) { post(fac.personaId, fac.personaName, fac.role, "neutral", text, is_error); };

    struct Asked {
        bool ok = false;
        std::string text;
        std::string error;
    };
    auto ask = [&](const std::string& location, const std::string& persona_name, const std::string& role, const PromptPair& p,
                   const std::optional<std::string>& use_model) {
        CostLogContext c;
        c.flow = args.flow;
        c.sessionId = args.sessionId;
        c.round = round;
        c.turn = turn;
        c.role = role;
        c.persona = persona_name;
        c.location = location;
        CostLoggedClient logged(llm, c);
        persona_engine::LLMInvokeArgs ia;
        ia.system = p.system;
        ia.prompt = p.prompt;
        ia.model = use_model;
        const auto res = logged.invoke(ia);
        return res.ok ? Asked{true, str::trim(res.text), ""} : Asked{false, "", res.error};
    };

    int reform = 0, conservative = 0;
    for (const auto& s : cast->speakers) {
        if (s.camp == "reform") ++reform;
        if (s.camp == "conservative") ++conservative;
    }
    log("未来予測 開始 (改革派 " + std::to_string(reform) + " 人 / 保守派 " + std::to_string(conservative) + " 人)");

    // ── [F1] 破壊可能な一手の提案 ──
    const auto proposal_res =
        ask("foresight-proposal", fac.personaName, fac.role, build_proposal_prompt(args.theme, args.conclusion, args.context), args.model);
    if (!proposal_res.ok || proposal_res.text.empty()) {
        const std::string reason = proposal_res.ok ? std::string("空応答") : proposal_res.error;
        warn("未来予測: 提案の生成に失敗 (" + reason + ")");
        post_fac("[エラー: 未来予測の提案を生成できませんでした (" + reason + ")]", true);
        result.status = "failed";
        result.failures = 1;
        return result;
    }
    const std::string proposal = proposal_res.text;
    post_fac("🔮 未来予測アドバイザリー (参考): 破壊的な一手の提案\n" + proposal, false);

    // ── [F2] 改革派 / 保守派の議論 (往復ごとに 成立条件 → 反証 → 確かめ方) ──
    std::vector<ForesightLine> lines;
    const auto order = build_camp_turn_order(cast->speakers, args.turns);
    for (std::size_t i = 0; i < order.size(); ++i) {
        const auto& speaker = order[i];
        CampTurnArgs ca;
        ca.theme = args.theme;
        ca.proposal = proposal;
        ca.personaName = speaker.personaName;
        ca.camp = speaker.camp;
        ca.lines = lines;
        ca.stage = camp_stage_at(static_cast<int>(i));
        ca.ownStatements = speaker.ownStatements;
        const auto res = ask("foresight-camp", speaker.personaName, speaker.role, build_camp_turn_prompt(ca), args.model);
        if (!res.ok) {
            result.failures++;
            warn("未来予測: " + speaker.personaName + " の発言に失敗 (" + res.error + ")");
            post(speaker.personaId, speaker.personaName, speaker.role, speaker.camp, "[エラー: " + res.error + "]", true);
            continue;
        }
        if (res.text.empty()) {
            result.failures++;
            warn("未来予測: " + speaker.personaName + " の発言が空応答");
            continue;
        }
        lines.push_back({speaker.personaName, speaker.camp, res.text});
        post(speaker.personaId, speaker.personaName, speaker.role, speaker.camp, res.text, false);
    }

    // ── [F3] 未解決の前提 + 3 / 5 / 10 年時点のシナリオ + ビジネス分析 ──
    ForecastArgs fa;
    fa.theme = args.theme;
    fa.conclusion = args.conclusion;
    fa.proposal = proposal;
    fa.lines = lines;
    fa.context = args.context;
    const auto forecast_res = ask("foresight-forecast", fac.personaName, fac.role, build_forecast_prompt(fa),
                                  args.forecastModel ? args.forecastModel : args.model);
    std::string forecast;
    if (forecast_res.ok && !forecast_res.text.empty()) {
        forecast = forecast_res.text;
        post_fac("🔭 未来予測アドバイザリー (参考): 3 / 5 / 10 年後\n" + forecast, false);
    } else {
        result.failures++;
        const std::string reason = forecast_res.ok ? std::string("空応答") : forecast_res.error;
        warn("未来予測: 予測の生成に失敗 (" + reason + ")");
        post_fac("[エラー: 未来予測を生成できませんでした (" + reason + ")]", true);
    }

    if (args.paperBody) {
        try {
            const auto current = args.paperBody->get ? args.paperBody->get() : std::nullopt;
            if (args.paperBody->set) args.paperBody->set(append_foresight_markdown(current.value_or(""), ForesightSection{proposal, forecast}));
        } catch (const std::exception& e) {
            result.failures++;
            warn(std::string("未来予測: ペーパーへの追記に失敗 (") + e.what() + ")");
        }
    }

    log("未来予測 終了 (" + std::to_string(result.utterances.size()) + " 発話、失敗 " + std::to_string(result.failures) + ")");
    result.proposal = proposal;
    result.forecast = forecast;
    result.status = result.failures > 0 ? "degraded" : "done";
    return result;
}

} // namespace di::flow::foresight
