#include "flow/facilitator_turn.hpp"

#include "base/crypto.hpp"
#include "base/strings.hpp"
#include "flow/cost_logger.hpp"

namespace di::flow {

std::string build_facilitator_prompt(const std::string& theme, int round, const std::optional<PreviousRoundContext>& previous_round,
                                     const std::vector<std::string>& guiding_issues) {
    // 承認済み論点 (09): ペーパーゲートで人間が確認した論点を毎ラウンド参考注入する。
    std::string issues_block;
    if (!guiding_issues.empty()) {
        issues_block = "事前に整理された論点 (参考):\n";
        for (std::size_t i = 0; i < guiding_issues.size(); ++i) {
            if (i > 0) issues_block += "\n";
            issues_block += std::to_string(i + 1) + ". " + guiding_issues[i];
        }
        issues_block += "\n";
    }
    const std::string base = "あなたは議論の進行役です。\nテーマ「" + theme + "」について、ラウンド " + std::to_string(round) +
                             " の議論を始めてください。\n" + issues_block;

    // round 1 は従来通り (文脈なし出題)。
    if (!previous_round) return base + "参加者が意見を出しやすいよう、1〜2 文で議題を提示してください。";

    const std::string auf_line =
        previous_round->aufhebung.empty() ? "" : "\n止揚: " + str::join(previous_round->aufhebung, " / ");
    const std::string winner_line =
        previous_round->winner ? "\n世論 (最多得票): " + previous_round->winner->personaName + ": " +
                                     str::js_slice(previous_round->winner->text, 0, 120)
                               : "";
    return base + "\n# 前ラウンドの結果\n" + previous_round->summary + auf_line + winner_line + "\n\n" + "# 指示\n" +
           "前ラウンドの到達点を踏まえ、まだ深まっていない論点・対立が残る論点へ\n" +
           "議論を進める出題を 1〜2 文で行ってください。前ラウンドの繰り返しは避ける。";
}

std::optional<FlowUtteranceRecord> run_facilitator_turn(persona_engine::LLMClient& llm, const FacilitatorTurnArgs& args) {
    CostLogContext ctx;
    ctx.flow = args.flow;
    ctx.sessionId = args.sessionId;
    ctx.round = args.round;
    ctx.turn = 0;
    ctx.role = args.facilitator.role;
    ctx.persona = args.facilitator.name;
    ctx.location = "facilitator";
    auto logged = with_cost_log(llm, std::move(ctx));

    persona_engine::LLMInvokeArgs call;
    call.prompt = build_facilitator_prompt(args.theme, args.round, args.previousRound, args.guidingIssues);
    call.model = args.facilitator.model;
    const auto result = logged.invoke(call);

    std::string text;
    bool is_error = false;
    if (!result.ok) {
        text = "[エラー: " + result.error + "]";
        is_error = true;
        if (args.warn) args.warn("ラウンド " + std::to_string(args.round) + " ファシリテーター開幕ターン エラー: " + result.error);
    } else {
        text = str::trim(result.text);
    }
    if (text.empty()) return std::nullopt;

    FlowUtteranceRecord r;
    r.id = crypto::uuid_v4();
    r.sessionId = args.sessionId;
    r.paperId = args.paperId;
    r.round = args.round;
    r.turn = 0;
    r.personaId = args.facilitator.id;
    r.personaName = args.facilitator.name;
    r.role = args.facilitator.role;
    r.stance = "neutral";
    r.text = std::move(text);
    r.isError = is_error;
    return r;
}

} // namespace di::flow
