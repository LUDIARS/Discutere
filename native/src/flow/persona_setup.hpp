#pragma once
// src/flow/persona-setup.ts の移植: ペルソナのセッション初期化 (respec 01, A9)。
// セッション開始時に 1 回だけ LLM を呼び、全ペルソナぶんの「価値軸 + 核主張」をまとめて生成し、
// キャストごと flow_session_persona に永続する。LLM 失敗・JSON 不正は価値軸なしで degrade。
#include <optional>
#include <string>
#include <vector>

#include "flow/effect_predict.hpp"
#include "flow/personas.hpp"
#include "persona_engine/llm/client.hpp"

namespace di::flow {

struct SetupSessionPersonasArgs {
    std::string theme;
    std::string sessionId;
    std::vector<FlowPersona> personas;
    std::string flow = "discussion";
    /** 生成に使うモデル ("" / 未指定ならメインモデル)。 */
    std::optional<std::string> model;
    /** 未指定は log::warn。 */
    WarnFn warn;
};

std::string build_persona_setup_prompt(const std::string& theme, const std::vector<FlowPersona>& personas);
/** キャスト全員を flow_session_persona に永続する。 */
void persist_session_personas(const std::string& session_id, const std::vector<FlowPersona>& personas);
/** 価値軸/核主張を割り当て (LLM 1 call) て永続し、マージしたペルソナ列を返す。 */
std::vector<FlowPersona> setup_session_personas(persona_engine::LLMClient& llm, const SetupSessionPersonasArgs& args);

} // namespace di::flow
