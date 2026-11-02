#include "flow/dialectic/scheduler.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>

#include "base/strings.hpp"

namespace di::flow::dialectic {

const std::vector<DialogueAct>& RESPOND_ACTS() {
    static const std::vector<DialogueAct> v{"support", "concede", "rebut", "question"};
    return v;
}
const std::vector<DialogueAct>& ATTACK_ACTS() {
    static const std::vector<DialogueAct> v{"rebut", "question"};
    return v;
}
const std::vector<DialogueAct>& FREE_ACTS() {
    static const std::vector<DialogueAct> v{"claim", "support", "rebut", "concede", "question"};
    return v;
}

Json to_json(const ScheduleDecision& d) {
    Json o = Json::object();
    o["kind"] = d.kind;
    o["personaId"] = d.personaId;
    o["targetUtteranceId"] = d.targetUtteranceId ? Json(*d.targetUtteranceId) : Json(nullptr);
    o["allowedActs"] = d.allowedActs;
    o["reason"] = d.reason;
    return o;
}

namespace {

const FlowPersona* min_speak_count_persona(const std::vector<const FlowPersona*>& candidates,
                                           const std::map<std::string, int>& counts) {
    const FlowPersona* best = nullptr;
    double best_count = std::numeric_limits<double>::infinity();
    for (const auto* p : candidates) {
        auto it = counts.find(p->id);
        const double c = it == counts.end() ? 0 : it->second;
        if (c < best_count) {
            best = p;
            best_count = c;
        }
    }
    return best;
}

} // namespace

std::optional<ScheduleDecision> schedule_next_turn(const ScheduleArgs& args) {
    if (!args.graph) throw std::invalid_argument("schedule_next_turn: graph is required");
    if (args.speakers.empty()) return std::nullopt;
    std::set<std::string> speaker_ids;
    for (const auto& p : args.speakers) speaker_ids.insert(p.id);

    // ① 未応答 rebut への応答権 (最古の未応答辺から。被反論者を指名)。
    for (const auto& edge : args.graph->unresolved_rebuttals()) {
        if (edge.targetPersonaId && speaker_ids.count(*edge.targetPersonaId))
            return ScheduleDecision{"respond", *edge.targetPersonaId, edge.rebutId, RESPOND_ACTS(), "未応答の反論に応答権を指名"};
    }

    // ② 攻撃番: 接続数最大の Position (同点は anchor 入力順) を反対陣営の発言数最少が攻める。
    const PositionAnchor* focal = nullptr;
    int focal_edges = -1;
    for (const auto& anchor : args.positionAnchors) {
        const int n = args.graph->edge_count_for(anchor.utteranceId);
        if (n > focal_edges) {
            focal = &anchor;
            focal_edges = n;
        }
    }
    if (focal) {
        std::vector<const FlowPersona*> opposing;
        for (const auto& p : args.speakers)
            if (p.role == "debater" && p.stance != focal->stance && p.id != focal->personaId) opposing.push_back(&p);
        if (const auto* attacker = min_speak_count_persona(opposing, args.speakCounts))
            return ScheduleDecision{"attack", attacker->id, focal->utteranceId, ATTACK_ACTS(),
                                    "対立の焦点 (接続数最大の Position) への攻撃番を指名"};
    }

    // ③ 発言数最少ペルソナの自由ターン (同点は speakers 入力順)。
    std::vector<const FlowPersona*> all;
    for (const auto& p : args.speakers) all.push_back(&p);
    const auto* free = min_speak_count_persona(all, args.speakCounts);
    if (!free) return std::nullopt;
    return ScheduleDecision{"free", free->id, std::nullopt, FREE_ACTS(), "発言数最少ペルソナの自由ターン"};
}

std::string nomination_fallback_text(const ScheduleDecision& decision, const std::string& persona_name,
                                     const std::optional<UtteranceDigest>& target) {
    if (decision.kind == "respond")
        return target ? target->personaName + "さんの「" + str::js_slice(target->text, 0, 60) + "」にまだ応答がありません。" +
                            persona_name + "さん、どう応えますか?"
                      : persona_name + "さん、先ほどの反論にどう応えますか?";
    if (decision.kind == "attack")
        return target ? persona_name + "さん、" + target->personaName + "さんの主張「" + str::js_slice(target->text, 0, 60) +
                            "」について、反対の立場から意見をどうぞ。"
                      : persona_name + "さん、反対の立場から意見をどうぞ。";
    if (decision.kind == "free") return persona_name + "さん、この論点について意見をどうぞ。";
    return "";
}

std::string render_facilitator_line(persona_engine::LLMClient& llm, const std::string& content,
                                    const std::optional<std::string>& model, const WarnFn& warn) {
    persona_engine::LLMInvokeArgs call;
    call.prompt = std::string("あなたは議論の進行役です。次の進行内容を、自然な口語 1〜2 文にしてください") +
                  "(意味を変えない・装飾やラベルを付けない):\n" + "内容: " + content;
    if (model && !model->empty()) call.model = *model;
    call.maxTokens = 300;
    const auto result = llm.invoke(call);
    if (!result.ok) {
        if (warn) warn("ファシリテーター進行文レンダリング失敗: " + result.error + " — 定型文で続行 (degrade)");
        return content;
    }
    const std::string text = str::trim(result.text);
    return text.empty() ? content : text;
}

std::string quote_excerpt(const std::string& text, std::size_t max) {
    const std::string t = str::squish(text);
    if (str::js_length(t) <= max) return t;
    const std::string head = str::js_slice(t, 0, static_cast<std::int64_t>(max));
    // 最後の文の区切り (。！？!?) を UTF-16 位置で探す。
    std::int64_t cut = -1;
    std::int64_t pos = 0;
    std::size_t i = 0;
    while (i < head.size()) {
        const auto cp = str::next_codepoint(head, i);
        if (cp == U'。' || cp == U'！' || cp == U'？' || cp == '!' || cp == '?') cut = pos;
        pos += cp >= 0x10000 ? 2 : 1;
    }
    return static_cast<double>(cut) >= static_cast<double>(max) / 3 ? str::js_slice(head, 0, cut + 1) : head + "…";
}

std::string nomination_frame_text(const ScheduleDecision& decision, const std::string& persona_name,
                                  const std::optional<UtteranceDigest>& target) {
    if (decision.kind == "respond")
        return target ? target->personaName + "さんの次の指摘に、まだ応答がありません。" + persona_name + "さん、どう応えますか?"
                      : persona_name + "さん、先ほどの反論にどう応えますか?";
    if (decision.kind == "attack")
        return target ? persona_name + "さん、" + target->personaName + "さんの次の主張について、反対の立場から意見をどうぞ。"
                      : persona_name + "さん、反対の立場から意見をどうぞ。";
    if (decision.kind == "free") return persona_name + "さん、この論点について意見をどうぞ。";
    return "";
}

std::string render_nomination(persona_engine::LLMClient& llm, const RenderNominationArgs& args) {
    const std::string line =
        render_facilitator_line(llm, nomination_frame_text(args.decision, args.personaName, args.target), args.model, args.warn);
    return args.target ? line + "\n> " + quote_excerpt(args.target->text) : line;
}

} // namespace di::flow::dialectic
