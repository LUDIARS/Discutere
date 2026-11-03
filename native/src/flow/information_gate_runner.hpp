#pragma once
// src/flow/information-gate-runner.ts の移植: 情報ゲートの呼び出し側グルー (web / discord 共通)。
//
// config 解決 + Core open/close + LLM の with_cost_log ラップ + 対象フロー判定をまとめ、
// run_information_gate を「議論/改善フロー開始の直前」に差し込めるようにする。
// ゲート無効 / 対象外フロー / Core 無し のときは nullopt を返す
// (呼び出し側は従来のカウント閾値 autoCrawl にフォールバックする)。
//
// 実行順 (09-paper-gate-debatability): 情報ゲート (量) → 議論適性ゲート (質) → 人間レビュー。
// 議論適性ゲートの config 解決 + with_cost_log ラップもここでまとめる
// (resolve_debatability_gate — 実体はペーパー草案構築 build_paper_draft の中で走る)。
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/index.hpp"
#include "flow/dispatch.hpp"
#include "flow/information_gate.hpp"
#include "flow/paper_review.hpp"
#include "flow/tags.hpp"
#include "flow/voice_cache.hpp"
#include "persona_engine/llm/client.hpp"

namespace di::flow {

/** Core を開く関数 (TS の openCore: () => Core)。 */
using CoreOpenFn = std::function<std::unique_ptr<core::Core>()>;

struct FlowGateArgs {
    FlowKind kind;
    std::string theme;
    std::vector<FlowTag> tags;
    /** 評価用 LLM (素の client。ここで with_cost_log する)。 */
    persona_engine::LLMClient* llm = nullptr;
    CoreOpenFn openCore;
    std::string workspaceId;
    ListExternalVoicesFn listExternalVoices;
    /** コストログ用キー (web=sessionId / discord=threadId)。 */
    std::string sessionId;
    std::function<void(const std::string&)> log;
    std::function<void(const std::string&)> warn;
    std::optional<std::string> youtubeApiKey;
    /** C++ 版の追加 (テスト用): 取得→取込 / 密度評価の差し替え。既定は本物 (密度評価は blackbox 経由)。 */
    CollectAndImportFn collectAndImportFn;
    EvaluateFn evaluateFn;
};

/**
 * 情報ゲートが有効なら議論/改善の開始前に実行する。
 * @returns 実行した場合は結果、ゲート対象外なら nullopt (= 従来 autoCrawl にフォールバック)。
 */
std::optional<InformationGateResult> gate_before_flow(const FlowGateArgs& args);

/** build_paper_draft に渡す議論適性ゲートの実行オプション (config 解決 + コストログ済み LLM)。 */
struct DebatabilityGateOptions {
    /** with_cost_log で包んだ LLM (この struct が持ち続ける)。 */
    std::shared_ptr<persona_engine::LLMClient> llm;
    int minArmableIssues = 2;
    /** build_paper_draft の deps.debatability に渡す形 (llm はこの struct が生きている間だけ有効)。 */
    PaperDebatabilityGate to_paper_gate() const;
};

/**
 * 議論適性ゲート (09) の実行オプションを解決する。
 * 無効 (config.flow.debatability.enabled=false) / 対象外フロー (学習・壁打ち) なら nullopt
 * (= ゲートを走らせない → 現行挙動と完全一致)。
 */
std::optional<DebatabilityGateOptions> resolve_debatability_gate(const FlowKind& kind, const std::string& session_id,
                                                                 persona_engine::LLMClient& llm);

} // namespace di::flow
