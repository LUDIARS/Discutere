#pragma once
// src/flow/foresight/after-flow.ts の移植: 議論/改善フローの結果に未来予測アドバイザリーを続ける (dispatch の後段)。
//
// オプショナル: 観点タグ「未来予測」を付けた議論だけで走る (neco 2026-10-18)。
// config.flow.foresight.enabled=false なら、タグがあっても走らない (機能ごと止める)。
// エンジン (rounds / dialectic) の結果 FlowDirectorResult だけを入力にするので、
// どちらのエンジンにも同じ経路で付く。結論は変えない参考扱い。
// フェーズ内の失敗は warn して本議論の結果をそのまま返す (議論を壊さない)。
//
// 進行状態 (state) を sessionId ごとに記録し、Web の status が本議論の完了と
// 未来予測の完了を分けて返せるようにする。
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "flow/director.hpp"
#include "flow/tags.hpp"
#include "flow/voice_cache.hpp"
#include "persona_engine/llm/client.hpp"

namespace di::flow::foresight {

/** prompt に載せる外部の声 1 件の上限 (字、UTF-16 単位)。 */
inline constexpr std::size_t VOICE_CHARS = 200;

struct ForesightAfterFlowDeps {
    persona_engine::LLMClient* llm = nullptr;
    std::function<void(const FlowUtteranceRecord&)> onUtterance;
    /** 本議論と同じ外部の声の検索 (無ければ声は渡さない)。 */
    ListExternalVoicesFn listExternalVoices;
    WarnFn log;
    WarnFn warn;
    /** 先に分かっている sessionId (Web)。本議論の実行中から「待機中」を記録する。 */
    std::optional<std::string> sessionId;
};

/** このタグ集合で未来予測を行うか (機能の有効化 + タグ)。 */
bool foresight_requested(const std::vector<FlowTag>& tags);

/**
 * 本議論の実行を包み、未来予測を続ける。sessionId が先に分かっている (Web) なら、本議論の実行中から
 * 「待機中」を記録して、結論が出た直後に完了と誤表示されないようにする。本議論の例外は状態を失敗にして投げ直す。
 */
FlowDirectorResult run_with_foresight(const std::string& theme, const std::vector<FlowTag>& tags, const std::string& flow,
                                      const std::function<FlowDirectorResult()>& run, const ForesightAfterFlowDeps& deps);

/** 本議論の結果に未来予測を続ける (要求が無い・未収束・話者不足なら結果をそのまま返す)。 */
FlowDirectorResult continue_with_foresight(const std::string& theme, const std::vector<FlowTag>& tags, const std::string& flow,
                                           FlowDirectorResult result, const ForesightAfterFlowDeps& deps);

} // namespace di::flow::foresight
