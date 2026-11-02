#pragma once
// src/flow/dialectic/position.ts の移植: 定立 [1] — issue ごとに debater の Position を生成する (respec 06 §2)。
// 構造化 JSON (claim + grounds 1〜3 + values) + 露出用口語文を同時に返させる。パース失敗・LLM 障害は degrade。
#include <optional>
#include <string>
#include <vector>

#include "flow/dialectic/store.hpp"
#include "flow/effect_predict.hpp"
#include "flow/personas.hpp"
#include "persona_engine/llm/client.hpp"

namespace di::flow::dialectic {

struct GeneratedPosition {
    std::string claim;
    std::optional<std::string> qualifier;
    std::vector<Ground> grounds;
    std::vector<std::string> values;
    std::string text;
    /** grounds と同じ順の論拠ごとの露出文。 */
    std::vector<std::string> groundTexts;
    bool degraded = false;
};
Json to_json(const GeneratedPosition& p);

struct BuildPositionPromptArgs {
    FlowPersona persona;
    std::string stance;
    IssueRecord issue;
    std::optional<std::string> voicesBlock;
    std::optional<std::string> feedback;
};
std::string build_position_prompt(const BuildPositionPromptArgs& args);

inline constexpr std::size_t GROUNDS_MAX = 3;
/** 根拠 id の採番 (stance 接頭辞で issue 内一意・決定的)。 */
std::string ground_id(const std::string& stance, std::size_t index);

struct GeneratePositionArgs {
    std::string theme;
    IssueRecord issue;
    FlowPersona persona;
    std::string stance;
    /** ペーパー base (system に固定)。 */
    std::string paperSystem;
    std::optional<std::string> voicesBlock;
    std::optional<std::string> feedback;
    std::optional<std::string> model;
    WarnFn warn;
};
/** Position を 1 つ生成する (LLM エラーは throw せず degrade)。 */
GeneratedPosition generate_position(persona_engine::LLMClient& llm, const GeneratePositionArgs& args);

} // namespace di::flow::dialectic
