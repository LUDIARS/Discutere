#pragma once
// src/slack/slack-live.ts の移植: 新フロー議論エンジン (flow) の Slack live アダプタ。
//
// discord-live の Slack 版。dispatch_flow を呼び、onUtterance を chat.postMessage に、
// onVote を reactions.add + 集計テキスト投稿に結線する。1 スレッド (thread_ts) = 1 議論。
//
//   - discussion / improvement : 完走型。結論を bot 投稿で締める。
//   - learning                 : 収集のみ。openCore (Core) が要る。
//   - sparring                 : 対話継続型。session を thread_ts で保持し、
//                                スレッド返信を submit_user へ橋渡しする。
//
// 表示名は compose_display_name のペルソナ名のみ (Slack の user 名/ID は保存しない)。
// C++ 版: start_slack_flow は同期 (議論を最後まで進める)。呼び出し側 (slack_router) が背景のプールに積む。
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "crawler/sentiment/cascade.hpp"
#include "flow/dispatch.hpp"
#include "flow/information_gate_runner.hpp"
#include "flow/tags.hpp"
#include "flow/voice_cache.hpp"
#include "persona_engine/llm/client.hpp"
#include "slack/web_api.hpp"

namespace di::slack {

/** 投票の可視化に使うリアクション名 (絵文字名、コロン無し)。 */
inline constexpr const char* VOTE_WINNER_REACTION = "trophy";
inline constexpr const char* VOTE_REACTION = "+1";

struct SlackFlowDeps {
    /** Web API 用 bot token (xoxb-)。ログに出さない。 */
    std::string botToken;
    /** フロー実行用 LLM クライアント。 */
    persona_engine::LLMClientPtr llm;
    /** learning が KG 書込に使う Core ファクトリ (learning を許可するなら必須)。 */
    flow::CoreOpenFn openCore;
    flow::ListExternalVoicesFn listExternalVoices;
    crawler::sentiment::CascadeClients sentimentClients;
    std::optional<std::string> gamesDir;
    std::optional<std::string> workspaceId;
    /** C++ 版の追加: Web API の取得先 (テストで偽サーバに向ける)。 */
    WebApiOptions webApi;
    /** C++ 版の追加 (テスト用): 議論ドライバの差し替え。 */
    flow::DispatchDeps::Drivers drivers;
};

struct StartSlackFlowInput {
    std::string channel;
    /** スレッド ID (= 親メッセージ ts)。 */
    std::string threadTs;
    /** スレッド = 議論の主題。 */
    std::string theme;
    flow::FlowKind flow;
    std::vector<flow::FlowTag> tags;
    /** discussion/improvement のラウンド数 (任意)。 */
    std::optional<double> rounds;
    /** discussion/improvement の 1 ラウンドあたりターン数 (任意)。 */
    std::optional<double> turnsPerRound;
    /** 壁打ち相手のプールペルソナ id/name (任意。sparring のみ)。 */
    std::optional<std::vector<std::string>> opponentPersonaIds;
};

struct SlackConcluded {
    std::string scene;
    std::string title;
    std::string summary;
};

/** 収束時フック (Discord 版 FlowLiveHooks と同形)。 */
struct SlackFlowHooks {
    std::function<void(const SlackConcluded&)> onConcluded;
};

/** テスト用: 登録セッションをクリアする (TS の _resetSlackLive)。 */
void reset_slack_live_for_test();

/** そのスレッドで進行中の壁打ちがあるか。 */
bool has_slack_sparring_session(const std::string& thread_ts);

/**
 * スレッド投稿を新フローで起動する。
 * discussion/improvement/learning は完走させ結論を投稿。sparring は session を登録して返信を待つ。
 * 例外は外へ出さない (warn + スレッドへのエラー案内)。
 */
void start_slack_flow(const StartSlackFlowInput& input, const SlackFlowDeps& deps, const SlackFlowHooks& hooks = {});

/**
 * 進行中の壁打ちスレッドへの返信を処理する。
 * @returns 壁打ちとして処理したら true (= 通常ルーティング不要)。
 */
bool handle_slack_flow_reply(const std::string& thread_ts, const std::string& channel, const std::string& text, const SlackFlowDeps& deps,
                             const SlackFlowHooks& hooks = {});

} // namespace di::slack
