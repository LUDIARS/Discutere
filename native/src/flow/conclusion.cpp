#include "flow/conclusion.hpp"

#include <algorithm>

#include "base/log.hpp"
#include "base/strings.hpp"
#include "base/time.hpp"
#include "flow/cost_logger.hpp"
#include "flow/db/connection.hpp"
#include "flow/discussion_paper.hpp"
#include "persona_engine/facilitator/prompts.hpp"
#include "visualize/conclusion_cache.hpp"

namespace di::flow {

namespace fac = persona_engine::facilitator;

namespace {

/** LLM 応答から結論 summary を取り出す (JSON パース + 非空検証)。失敗は nullopt。 */
std::optional<std::string> try_parse_summary(const std::string& text) {
    try {
        const Json parsed = fac::extract_json_object(text);
        if (parsed.is_object() && parsed.contains("summary") && parsed["summary"].is_string()) {
            std::string s = parsed["summary"].get<std::string>();
            if (!str::trim(s).empty()) return s;
        }
        return std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace

std::string build_rounds_section(const std::vector<RoundSummary>& rounds) {
    if (rounds.empty()) return "";
    std::vector<std::string> lines;
    for (const auto& r : rounds) {
        const std::string auf = r.aufhebung.empty() ? "" : "\n  止揚: " + str::join(r.aufhebung, " / ");
        lines.push_back("ラウンド " + std::to_string(r.round) + ": " + r.summary + auf);
    }
    return "# 各ラウンドの要約\n" + str::join(lines, "\n") + "\n\n";
}

ConclusionResult generate_conclusion(persona_engine::LLMClient& llm, const GenerateConclusionArgs& args) {
    const WarnFn warn = args.warn ? args.warn : WarnFn([](const std::string& m) { log::warn(m); });

    // 世論 (投票で選ばれた発話 = winner utterance)。Set の挿入順を保つ。
    std::vector<std::string> winner_ids;
    for (const auto& r : args.voteResults)
        if (r.winner && std::find(winner_ids.begin(), winner_ids.end(), *r.winner) == winner_ids.end()) winner_ids.push_back(*r.winner);
    std::vector<fac::RecentUtterance> top_utterances;
    for (const auto& u : args.allUtterances)
        if (std::find(winner_ids.begin(), winner_ids.end(), u.id) != winner_ids.end()) top_utterances.push_back({u.personaName, u.text});

    std::vector<fac::RecentUtterance> recent_all;
    const std::size_t from = args.allUtterances.size() > 20 ? args.allUtterances.size() - 20 : 0;
    for (std::size_t i = from; i < args.allUtterances.size(); ++i)
        recent_all.push_back({args.allUtterances[i].personaName, args.allUtterances[i].text});

    std::vector<std::string> top_opinions;
    for (const auto& u : top_utterances) top_opinions.push_back(u.speaker + ": " + u.content);
    const auto converge = fac::build_converge_prompt(args.theme, recent_all, args.allAufhebung, top_opinions);

    CostLogContext ctx;
    ctx.flow = args.flow;
    ctx.sessionId = args.sessionId;
    ctx.location = "summary";
    auto logged = with_cost_log(llm, std::move(ctx));

    std::string summary = "(結論なし)";
    bool concluded = false;

    if (!args.allAufhebung.empty() || !top_utterances.empty()) {
        // 全ラウンドサマリを「# 各ラウンドの要約」節として結論入力に含める (A5)。
        persona_engine::LLMInvokeArgs call;
        call.prompt = converge.system + "\n\n" + build_rounds_section(args.rounds) + converge.user;
        const auto result = logged.invoke(call);
        if (result.ok) {
            // concluded 判定の統一 (A7): 構造化出力の失敗は 1 回だけリトライし、
            // それでも失敗なら生テキストを【収束(未検証)】として保存するが concluded=false。
            auto parsed = try_parse_summary(result.text);
            std::string raw_text = str::trim(result.text);
            if (!parsed) {
                warn("結論生成 JSON パース失敗 — 同一プロンプトで 1 回リトライ");
                const auto retry = logged.invoke(call);
                if (retry.ok) {
                    parsed = try_parse_summary(retry.text);
                    const std::string t = str::trim(retry.text);
                    if (!t.empty()) raw_text = t;
                } else {
                    warn("結論生成 リトライ失敗: " + retry.error);
                }
            }
            if (parsed) {
                summary = std::string(CONVERGE_PREFIX) + *parsed;
                concluded = true;
            } else if (!raw_text.empty()) {
                summary = std::string(UNVERIFIED_PREFIX) + raw_text;
                concluded = false;
                warn("結論生成: 構造化出力に 2 回失敗 — 生テキストを " + std::string(UNVERIFIED_PREFIX) + " として保存 (concluded=false)");
            }
        } else {
            warn("結論生成 失敗: " + result.error);
        }
    }

    // 注記 (議論適性: 低 の強行など) を結論に併記する (表示・永続の両方に乗る)。
    if (args.annotation && !str::trim(*args.annotation).empty()) summary = summary + "\n\n※ " + str::trim(*args.annotation);

    // DB に保存
    auto db = db::get_flow_db();
    const std::int64_t now = time::now_ms();
    db->run(R"SQL(INSERT OR REPLACE INTO flow_conclusion
       (session_id, paper_id, summary, aufhebung_json, top_utterance_ids_json, concluded, created_at)
     VALUES (?, ?, ?, ?, ?, ?, ?))SQL",
            {args.sessionId, args.paperId, summary, json_stringify(Json(args.allAufhebung)), json_stringify(Json(winner_ids)),
             concluded ? 1 : 0, now});
    TitleCacheMeta meta;
    meta.discussionType = args.flow;
    upsert_discussion_title_cache(args.sessionId, concluded ? summary : args.theme, concluded ? "conclusion" : "paper", now, meta);

    // 結論一覧キャッシュへ write-through (議論が収束するたびに発話数=議論ボリュームを更新)。KG を引かない軽量更新。
    try {
        visualize::WriteThroughParams p;
        p.sessionId = args.sessionId;
        p.title = args.theme;
        if (concluded) p.conclusion = str::trim(str::js_slice(summary, static_cast<std::int64_t>(str::js_length(CONVERGE_PREFIX))));
        p.utteranceCount = static_cast<std::int64_t>(args.allUtterances.size());
        p.aufhebungCount = static_cast<std::int64_t>(args.allAufhebung.size());
        p.updatedAt = now;
        visualize::write_through_flow_conclusion(*db, p);
    } catch (const std::exception& e) {
        warn(std::string("結論キャッシュ更新 失敗 (議論は継続): ") + e.what());
    }
    return {summary, concluded};
}

} // namespace di::flow
