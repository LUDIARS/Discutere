#pragma once
// src/flow/dispatch.ts の移植: 共通ディスパッチ (t7-entrypoints.md / OVERVIEW §9)。
//
// (theme, tags, flow, scene) を受け、議論タイプ (flow) に応じて対応するドライバを起動する
// 単一入口。WebUI / Discord フォーラム投稿 / Slack の全経路がここに集約する。
//
//   - 議論 (discussion) → run_discussion_flow (中立投票) / flow.engine=dialectic なら run_dialectic_discussion_flow
//   - 改善 (improvement) → run_improvement_flow (design_gap 機械スコア) / dialectic なら run_dialectic_improvement_flow
//   - 学習 (learning)   → run_learning_flow (収集のみ・LLM 議論なし)
//   - 壁打ち (sparring)  → SparringSession (ユーザターン起点・対話継続)
//
// 議論タイプは **必須**。未指定/不正な投稿は受理しない (parse_flow_kind が nullopt → FlowTypeRequiredError)。
// 観点タグ「代理」で参加者を解決できたら engine 設定に依らず dialectic の代理モードで回す。
// 観点タグ「未来予測」なら本議論の後に未来予測アドバイザリーを続ける (foresight/after_flow)。
//
// C++ 版の使い方: dispatch_flow は呼んだスレッドで議論を最後まで進める同期 API。HTTP / Gateway のハンドラからは
// background_executor().submit に渡して呼ぶ (TS の `void dispatchFlow(...)`)。進行の問い合わせと停止は
// deps.state (FlowRunState) を渡し、別スレッドから snapshot() / request_stop() を呼ぶ (rounds / dialectic 両方)。
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "core/index.hpp"
#include "crawler/sentiment/cascade.hpp"
#include "flow/director.hpp"
#include "flow/games_md.hpp"
#include "flow/learning.hpp"
#include "flow/sparring.hpp"
#include "flow/tags.hpp"
#include "persona_engine/llm/client.hpp"

namespace di::flow {

/** "discussion" | "improvement" | "learning" | "sparring" */
using FlowKind = std::string;

/**
 * ラベル文字列を FlowKind に解決する。完全一致を優先し、無ければ部分一致 (長い別名を優先)。
 * 解決できなければ nullopt (= 議論タイプ未指定/不正)。学習・壁打ちのラベルは持たない (TS と同じ)。
 */
std::optional<FlowKind> parse_flow_kind(const std::optional<std::string_view>& label);

/** 議論/改善ドライバの形 (テスト用の差し替え)。 */
using FlowDriverFn = std::function<FlowDirectorResult(persona_engine::LLMClient& llm, const std::string& theme,
                                                      const std::vector<FlowTag>& tags, const FlowDirectorDeps& deps)>;
using LearningDriverFn =
    std::function<LearningFlowResult(const std::string& game_title, const std::string& theme, const LearningFlowDeps& deps)>;

struct DispatchDeps {
    /** 議論・未来予測・壁打ちの LLM。壁打ちのセッションが持ち続けるので shared_ptr。必須。 */
    persona_engine::LLMClientPtr llm;
    /** Discatier Core (learning が KG/メカニクス書込に使う。learning を使うなら必須)。 */
    core::Core* core = nullptr;
    ListExternalVoicesFn listExternalVoices;
    YoutubeSearchFn youtubeSearch;
    /** learning フロー入力 (収集済み匿名意見 / メカニクス)。 */
    std::vector<LearningOpinion> opinions;
    std::vector<GameMechanicEntry> mechanics;
    /** learning 自動収集モード (① 類似ゲームの自動収集)。 */
    std::optional<LearningCrawlSpec> learningCrawl;
    /** learning の感情カスケード LLM。 */
    crawler::sentiment::CascadeClients sentimentClients;
    /** learning の slug 明示 (日本語タイトル用)。 */
    std::optional<std::string> slug;
    /** 壁打ちの 1 ユーザ発話あたり応答数。 */
    std::optional<int> responsesPerTurn;
    std::function<void(const FlowUtteranceRecord&)> onUtterance;
    /** ラウンド投票後の通知 (item3: リアクションで得票を可視化)。discussion/improvement のみ。 */
    std::function<void(const VoteEvent&)> onVote;
    Rng rng;
    std::optional<std::string> gamesDir;
    std::optional<std::string> workspaceId;
    /** WebUI が起動前に id を返すための固定 sessionId (discussion/improvement)。 */
    std::optional<std::string> sessionId;
    /** 確定済みディスカッションペーパー (discussion/improvement のみ反映。指定時は investigate を省略)。 */
    std::optional<PaperOverride> paperOverride;
    WarnFn log;
    WarnFn warn;
    /** 進行状態 (C++ 版の追加。discussion/improvement の進行の問い合わせと停止)。 */
    std::shared_ptr<FlowRunState> state;
    /**
     * テスト用にドライバを差し替える (既定は本物)。engine 設定より優先する。
     * 壁打ち (sparring) は SparringSession を直接構築するため差し替え対象外。
     */
    struct Drivers {
        FlowDriverFn discussion;
        FlowDriverFn improvement;
        LearningDriverFn learning;
    } drivers;
};

struct DispatchInput {
    std::string theme;
    std::vector<FlowTag> tags;
    /** 議論タイプ (ラベル or FlowKind)。必須。 */
    std::string flow;
    /** トランスポート由来の scene (web:<room> / discord:<guild>/<thread>)。記録用。 */
    std::optional<std::string> scene;
    /** この議論のラウンド数 (省略時 config.flow.rounds)。discussion/improvement のみ反映。 */
    std::optional<double> rounds;
    /** この議論の 1 ラウンドあたりターン数 (省略時 config.flow.turnsPerRound)。discussion/improvement のみ反映。 */
    std::optional<double> turnsPerRound;
    /** 議論/改善キャストに含める生成済みペルソナの id/name。 */
    std::optional<std::vector<std::string>> personaIds;
    /** 壁打ち相手に充てるプールペルソナの id/name (G。sparring のみ反映)。 */
    std::optional<std::vector<std::string>> opponentPersonaIds;
};

/** kind = "discussion" | "improvement" → result / "learning" → learning / "sparring" → session。 */
struct DispatchResult {
    FlowKind kind;
    FlowKind flow;
    std::optional<FlowDirectorResult> result;
    std::optional<LearningFlowResult> learning;
    std::shared_ptr<SparringSession> session;
};

/** flow が未指定/不正のとき投げる (議論タイプは必須)。 */
class FlowTypeRequiredError : public std::runtime_error {
public:
    explicit FlowTypeRequiredError(const std::string& label);
};

/**
 * フローを起動する。flow=壁打ち のときは起動済み SparringSession を返す (対話継続用)。
 * 他フローは完走させて結果を返す。代理の指定が読めなければ proxy::ProxyRequestError を投げる。
 */
DispatchResult dispatch_flow(const DispatchInput& input, const DispatchDeps& deps);

} // namespace di::flow
