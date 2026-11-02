#include "flow/round_summary.hpp"

#include "base/log.hpp"
#include "base/strings.hpp"
#include "base/time.hpp"
#include "flow/cost_logger.hpp"
#include "flow/db/connection.hpp"
#include "persona_engine/facilitator/prompts.hpp"

namespace di::flow {

std::string build_round_summary_prompt(const std::string& theme, const std::vector<NamedUtterance>& utterances) {
    std::vector<std::string> lines;
    for (const auto& u : utterances) lines.push_back("- " + u.personaName + ": " + u.text);
    return "議題: " + theme + "\n\n以下はこのラウンドで出た意見です:\n" + str::join(lines, "\n") + "\n\n" +
           "このラウンドの議論を 2〜3 文で要約してください。対立点と共通点を明確にする。\n" +
           "返答はサマリテキストのみ (JSON 不要)。";
}

RoundSummaryOutput summarize_round(persona_engine::LLMClient& llm, const RoundSummaryInput& input) {
    const WarnFn warn = input.warn ? input.warn : WarnFn([](const std::string& m) { log::warn(m); });
    const std::string round = std::to_string(input.round);
    std::vector<persona_engine::facilitator::RecentUtterance> recent;
    for (const auto& u : input.utterances) recent.push_back({u.personaName, u.text});

    // ── サマリ生成 ──
    std::string summary = "ラウンド " + round + " 完了 (" + std::to_string(input.utterances.size()) + " 発話)";
    CostLogContext ctx{input.flow, input.sessionId, static_cast<double>(input.round), {}, {}, {}, "summary", {}};
    auto summary_logged = with_cost_log(llm, ctx);
    persona_engine::LLMInvokeArgs summary_call;
    summary_call.prompt = build_round_summary_prompt(input.theme, input.utterances);
    const auto summary_result = summary_logged.invoke(summary_call);
    if (summary_result.ok && !str::trim(summary_result.text).empty()) {
        summary = str::trim(summary_result.text);
    } else if (!summary_result.ok) {
        warn("サマリ生成 round=" + round + " 失敗: " + summary_result.error);
    }

    // ── 止揚検出 ──
    std::vector<std::string> new_aufhebung;
    if (input.utterances.size() >= 2) {
        const auto auf_prompt =
            persona_engine::facilitator::build_aufhebung_judge_prompt(input.theme, recent, input.stockedAufhebung);
        auto auf_logged = with_cost_log(llm, ctx);
        persona_engine::LLMInvokeArgs auf_call;
        auf_call.prompt = auf_prompt.system + "\n\n" + auf_prompt.user;
        const auto auf_result = auf_logged.invoke(auf_call);
        if (auf_result.ok) {
            try {
                const Json parsed = persona_engine::facilitator::extract_json_object(auf_result.text);
                const Json& found = parsed.is_object() && parsed.contains("found") ? parsed["found"] : Json();
                const Json& s = parsed.is_object() && parsed.contains("summary") ? parsed["summary"] : Json();
                if (json_truthy(found) && json_truthy(s))
                    new_aufhebung.push_back(s.is_string() ? s.get<std::string>() : json_stringify(s));
            } catch (const std::exception&) {
                // JSON パース失敗は「止揚なし」として続行するが、無言では捨てない (respec 04, A7)。
                warn("止揚検出 round=" + round + ": JSON パース失敗 — 止揚なしとして続行 (" +
                     str::js_slice(str::trim(auf_result.text), 0, 60) + ")");
            }
        } else {
            warn("止揚検出 round=" + round + " 失敗: " + auf_result.error);
        }
    }

    RoundSummaryOutput out;
    out.summary = summary;
    out.aufhebung = input.stockedAufhebung;
    out.aufhebung.insert(out.aufhebung.end(), new_aufhebung.begin(), new_aufhebung.end());
    out.newAufhebung = new_aufhebung;

    // ── DB 追記 ── discussion_paper_round を更新 (T2 の placeholder を上書き)。行が無ければ挿入 (冪等)。
    auto db = db::get_flow_db();
    const std::string auf_json = json_stringify(Json(new_aufhebung));
    db->run("UPDATE discussion_paper_round SET summary = ?, aufhebung_json = ? WHERE paper_id = ? AND round = ?",
            {summary, auf_json, input.paperId, input.round});
    if (!db->get("SELECT id FROM discussion_paper_round WHERE paper_id = ? AND round = ?", {input.paperId, input.round})) {
        db->run("INSERT INTO discussion_paper_round (paper_id, round, summary, aufhebung_json, created_at) VALUES (?, ?, ?, ?, ?)",
                {input.paperId, input.round, summary, auf_json, time::now_ms()});
    }
    return out;
}

} // namespace di::flow
