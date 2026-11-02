#pragma once
// src/flow/dialectic/scheduler.ts の移植: dialectic スケジューラ — 決定的コード (respec 06 §3)。
//   ① 未応答 rebut への応答権 ② 攻撃番 (接続数最大の Position を反対陣営の発言数最少が攻める) ③ 自由ターン
// LLM は指名を自然な進行文にレンダリングするだけ (失敗は決定的な定型文)。
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "flow/argument_graph.hpp"
#include "flow/dialectic/turn_prompt.hpp"
#include "flow/effect_predict.hpp"
#include "flow/personas.hpp"
#include "persona_engine/llm/client.hpp"

namespace di::flow::dialectic {

const std::vector<DialogueAct>& RESPOND_ACTS();
const std::vector<DialogueAct>& ATTACK_ACTS();
const std::vector<DialogueAct>& FREE_ACTS();

struct PositionAnchor {
    std::string personaId;
    std::string stance;
    std::string utteranceId;
};

struct ScheduleArgs {
    const ArgumentGraph* graph = nullptr;
    std::vector<FlowPersona> speakers;
    std::vector<PositionAnchor> positionAnchors;
    std::map<std::string, int> speakCounts;
};

struct ScheduleDecision {
    /** "respond" | "attack" | "free" */
    std::string kind;
    std::string personaId;
    std::optional<std::string> targetUtteranceId;
    std::vector<DialogueAct> allowedActs;
    std::string reason;
};
Json to_json(const ScheduleDecision& d);

/** 次の一手を決定的に決める。発言者がいなければ nullopt。graph は必須。 */
std::optional<ScheduleDecision> schedule_next_turn(const ScheduleArgs& args);

std::string nomination_fallback_text(const ScheduleDecision& decision, const std::string& persona_name,
                                     const std::optional<UtteranceDigest>& target = std::nullopt);
/** 進行内容を自然な進行文にレンダリングする。失敗 / 空応答は content そのまま。 */
std::string render_facilitator_line(persona_engine::LLMClient& llm, const std::string& content,
                                    const std::optional<std::string>& model = std::nullopt, const WarnFn& warn = {});

inline constexpr std::size_t QUOTE_MAX = 160;
/** 発言を引用用に切り出す (上限を超えたら文の区切りまで、無ければ上限で切って「…」)。 */
std::string quote_excerpt(const std::string& text, std::size_t max = QUOTE_MAX);
std::string nomination_frame_text(const ScheduleDecision& decision, const std::string& persona_name,
                                  const std::optional<UtteranceDigest>& target = std::nullopt);

struct RenderNominationArgs {
    ScheduleDecision decision;
    std::string personaName;
    std::optional<UtteranceDigest> target;
    std::optional<std::string> model;
    WarnFn warn;
};
std::string render_nomination(persona_engine::LLMClient& llm, const RenderNominationArgs& args);

} // namespace di::flow::dialectic
