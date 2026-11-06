#include "flow/persona_turn.hpp"

#include <stdexcept>

#include "base/crypto.hpp"
#include "base/strings.hpp"
#include "config.hpp"
#include "flow/cost_logger.hpp"

namespace di::flow {

std::optional<FlowUtteranceRecord> run_persona_turn(persona_engine::LLMClient& llm, const PersonaTurnArgs& args) {
    if (!args.paper) throw std::invalid_argument("run_persona_turn: paper is required");
    if (!args.voiceCache) throw std::invalid_argument("run_persona_turn: voiceCache is required");
    const auto& cfg = config::get_config();
    const auto& persona = args.persona;
    const auto& paper = *args.paper;
    const FlowStance stance = persona_stance(persona);

    // ユーザ意見 RAG (item5: セッションキャッシュ経由で全ペルソナ共有)
    std::vector<ContextVoice> user_voices =
        args.voiceCache->lookup({paper.theme}, static_cast<int>(cfg.flow.paperRichness.voices));

    // YouTube コメントを userVoices に追加 (補完時のみ。確定ペーパー経路は investigation=null)
    if (args.investigation && args.investigation->youtubeUsed && !args.investigation->youtubeComments.empty()) {
        const auto& comments = args.investigation->youtubeComments;
        for (std::size_t i = 0; i < comments.size() && i < 3; ++i) user_voices.push_back({comments[i], "youtube", std::nullopt});
    }

    // ペーパー組み立て
    BuildPersonaPaperArgs pa;
    pa.paper = paper;
    pa.persona = persona;
    pa.stance = stance;
    pa.currentRoundUtterances = args.roundUtterances;
    pa.userVoices = std::move(user_voices);
    pa.syntheticOpinions = args.syntheticOpinions;
    const PersonaPaper persona_paper = build_persona_paper(pa);
    // 安定部 (議題/メカニクス) は system に置き session 内再利用 (E)。可変部 + persona 固有は user メッセージへ。
    const std::string persona_system = build_paper_system(persona_paper);
    const std::string prompt = build_persona_user_prompt(persona_paper, stance, persona);

    // LLM 呼び出し (コストログ付き)
    CostLogContext ctx;
    ctx.flow = args.flow;
    ctx.sessionId = args.sessionId;
    ctx.round = args.round;
    ctx.turn = args.turn;
    ctx.role = persona.role;
    ctx.persona = persona.name;
    ctx.location = persona.role == "facilitator" ? "facilitator" : "utterance";
    auto logged = with_cost_log(llm, std::move(ctx));

    persona_engine::LLMInvokeArgs call;
    call.system = persona_system;
    call.prompt = prompt;
    call.model = persona.model;
    const auto result = logged.invoke(call);

    std::string utterance_text;
    bool is_error = false;
    const std::string where = std::to_string(args.round) + "-" + std::to_string(args.turn) + " (" + persona.name + ")";
    if (!result.ok) {
        utterance_text = "[エラー: " + result.error + "]";
        is_error = true;
        if (args.warn) args.warn("ターン " + where + " LLM エラー: " + result.error);
    } else {
        utterance_text = str::trim(result.text);
        // 空応答は発話に数えない
        if (utterance_text.empty()) {
            if (args.log) args.log("ターン " + where + ": 空応答 (スキップ)");
            return std::nullopt;
        }
    }

    FlowUtteranceRecord r;
    r.id = crypto::uuid_v4();
    r.sessionId = args.sessionId;
    r.paperId = args.paperId;
    r.round = args.round;
    r.turn = args.turn;
    r.personaId = persona.id;
    r.personaName = persona.name;
    r.role = persona.role;
    r.stance = stance;
    r.text = std::move(utterance_text);
    r.isError = is_error;
    if (persona.possession) r.possessionName = persona.possession->label;
    return r;
}

} // namespace di::flow
