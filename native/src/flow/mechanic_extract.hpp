#pragma once
// src/flow/mechanic-extract.ts の移植: メカニクス LLM 増補。
// 集めた感想 + 既存メカニクスを LLM に渡し、語られている遊びのメカニクスを目標件数まで追加抽出する。
// 既存件数が目標以上なら LLM を呼ばない。LLM 失敗 / JSON 解析失敗は既存のまま (degrade)。
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.hpp"
#include "flow/effect_predict.hpp"
#include "flow/investigate.hpp"
#include "persona_engine/llm/client.hpp"

namespace di::flow {

/**
 * 文字列から最初の JSON 配列を取り出す (コードフェンス/前置き混入に耐える)。
 * 全体が読めなければ配列内の {...} を 1 個ずつブレースマッチで救済する (壊れたものは捨てる)。
 * 何も取れなければ nullopt。
 */
std::optional<Json> extract_json_array(std::string_view raw);

struct EnrichVoice {
    std::string content;
    std::optional<std::string> source;
};

struct EnrichMechanicsArgs {
    std::string theme;
    std::vector<MechanicSummary> existing;
    std::vector<EnrichVoice> voices;
    /** 目標件数。既存がこれ以上なら LLM を呼ばない。 */
    int target = 30;
    std::optional<std::string> model;
    WarnFn warn;
};

/** メカニクスを目標件数まで LLM で増補する。失敗時は既存のまま。 */
std::vector<MechanicSummary> enrich_mechanics(persona_engine::LLMClient& llm, const EnrichMechanicsArgs& args);

} // namespace di::flow
