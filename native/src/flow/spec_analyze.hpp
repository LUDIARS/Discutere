#pragma once
// src/flow/spec-analyze.ts の移植: 仕様書の解析学習 (② 仕様書の解析学習, LLM)。
// 貼り付けられたゲーム仕様書テキストを LLM に渡し、遊びのメカニクスを GameMechanicEntry[] として抽出する。
// 長い仕様書はチャンク分割 (16000 文字・重複 2000・最大 4 チャンク)。失敗は空配列で degrade。
#include <optional>
#include <string>
#include <vector>

#include "flow/effect_predict.hpp"
#include "flow/games_md.hpp"
#include "persona_engine/llm/client.hpp"

namespace di::flow {

struct AnalyzeSpecMechanicsArgs {
    std::string theme;
    std::string specText;
    /** 抽出上限 (既定 40)。 */
    int maxMechanics = 40;
    std::optional<std::string> model;
    WarnFn warn;
};

/** 仕様書テキストを 16000 文字 (UTF-16 単位) のチャンクに分割する (重複 2000、最大 4)。 */
std::vector<std::string> split_spec_into_chunks(const std::string& text);

/** 仕様書テキストから遊びのメカニクスを LLM で抽出する。specText が空なら LLM を呼ばず []。 */
std::vector<GameMechanicEntry> analyze_spec_mechanics(persona_engine::LLMClient& llm, const AnalyzeSpecMechanicsArgs& args);

} // namespace di::flow
