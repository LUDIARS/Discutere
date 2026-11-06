#pragma once
// src/flow/discord-live.ts の移植: 新フロー議論エンジン (src/flow) の Discord live アダプタ。
//
// 旧 auto-discussion (persona-engine + design_gap) に代わり、フォーラム議論を dispatch_flow
// (議論/改善/学習/壁打ち) で回す。onUtterance を webhook 投稿に繋いで発話を persona 名でスレッドに流し、
// 結論を bot 名義で締める。
//
//   - discussion / improvement : 完走型。ラウンドを回して結論を投稿。
//   - learning                 : 収集のみ (LLM 議論なし)。Core が要る。
//   - sparring                 : 対話継続型。session を threadId で保持し、スレッドへの返信を submit_user へ橋渡しする。
//
// 出力先 (webhook / bot post) は discord_hook/poster に委譲し、フローのロジックは dispatch に委譲する。
//
// C++ 版の使い方 (TS の async → 同期):
//   - start_forum_flow / handle_* は呼んだスレッドで最後まで進める (LLM・REST を待つ)。Gateway は
//     background executor に渡して呼ぶ (TS の `void startForumFlow(...)`)。
//   - ペーパー承認後の議論の完走は待たない (TS と同じ): deps.submitBackground (既定 background_executor) に積む。
//   - スレッドごとの直列化は paper_review_session の run_serial (届いた順に 1 つずつ)。
//   - モジュール内の状態 (壁打ち・レビュー待ち・声の検索・進行中の議論) は mutex で守る。
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/index.hpp"
#include "crawler/sentiment/cascade.hpp"
#include "discord_hook/discord_api.hpp"
#include "flow/dispatch.hpp"
#include "flow/paper_review.hpp"
#include "flow/tags.hpp"
#include "flow/user_voices/preparation.hpp"
#include "flow/user_voices/runtime.hpp"
#include "flow/voice_cache.hpp"
#include "persona_engine/llm/client.hpp"

namespace di::flow {

/** 投票の可視化に使う絵文字 (item3)。 */
inline constexpr const char* VOTE_WINNER_EMOJI = "🏆";
inline constexpr const char* VOTE_EMOJI = "👍";

struct FlowDiscordDeps {
    /** Voluptas への受付依頼 (未設定なら configured_voice_request)。 */
    std::optional<user_voices::VoiceRequestPort> voiceRequest;
    /** webhook / message 投稿用の Discord REST (TS の botToken)。必須。 */
    std::shared_ptr<discord_hook::DiscordApi> api;
    /** フロー実行用 LLM クライアント。必須。 */
    persona_engine::LLMClientPtr llm;
    /** learning が KG 書込に使う Core ファクトリ (learning を許可するなら必須)。 */
    std::function<std::unique_ptr<core::Core>()> openCore;
    ListExternalVoicesFn listExternalVoices;
    crawler::sentiment::CascadeClients sentimentClients;
    std::optional<std::string> gamesDir;
    std::optional<std::string> workspaceId;
    std::optional<std::string> youtubeApiKey;
    std::function<std::optional<std::string>()> getYoutubeApiKey;

    // ── C++ 版の注入点 (既定は本物) ──
    /** 投げっぱなしの仕事 (承認後の議論の完走・自動開始)。false = 満杯で積めなかった。既定 background_executor().submit。 */
    std::function<bool(std::function<void()>)> submitBackground;
    /** フローの起動 (既定 dispatch_flow)。 */
    std::function<DispatchResult(const DispatchInput&, const DispatchDeps&)> dispatch;
    /** ユーザーの声の収集 (既定 user_voices::prepare_user_voices。Steam / Voluptas へ出る)。 */
    std::function<user_voices::PreparedUserVoices(const user_voices::PrepareUserVoicesArgs&)> prepareUserVoices;
    /** Notion リンクの展開 (既定 expand_notion_links。Canalis の sidecar を起こす)。 */
    std::function<std::string(const std::string& text)> expandNotionLinks;
};

struct StartForumFlowInput {
    std::string guildId;
    std::string threadId;
    /** スレッド = 議論の主題。 */
    std::string theme;
    FlowKind flow;
    std::vector<FlowTag> tags;
    /** 議論ごとのラウンド数 (任意。discussion/improvement のみ反映、未指定は config 既定)。 */
    std::optional<double> rounds;
    /** 議論ごとの 1 ラウンドあたりターン数 (任意。discussion/improvement のみ反映)。 */
    std::optional<double> turnsPerRound;
    /** 壁打ち相手のプールペルソナ id/name (任意。sparring のみ反映)。 */
    std::optional<std::vector<std::string>> opponentPersonaIds;
    /** starter 本文 (任意。タイトルと別物のときだけ渡る)。学習の仕様書解析 ② と Notion リンクの材料。 */
    std::optional<std::string> specText;
    /** starter のテキスト系添付ファイル URL (任意。学習で仕様書解析 ③ に使う)。 */
    std::optional<std::vector<std::string>> specAttachmentUrls;
    /** 類似ゲーム (ユーザーの声をゲームごとに均等に混ぜる)。 */
    std::optional<std::vector<std::string>> similarGames;
};

/** 収束時フック (gateway が finalize_forum_post に結線する)。 */
struct FlowLiveHooks {
    std::function<void(const std::string& scene, const std::string& title, const std::string& summary)> onConcluded;
    /** 議論適性ゲート (09) が「議論不適」を出したときのフロー再提案 (gateway が flow-pick の再提示に結線する)。 */
    std::function<void(const std::string& guildId, const std::string& threadId, const std::string& theme,
                       const std::vector<FlowTag>& tags)>
        onReproposeFlowType;
};

/** テスト用: 登録セッション・レビュー待ち・声の検索・進行中の議論をクリアする (TS の _resetFlowLive)。 */
void reset_flow_live_for_test();

/** そのスレッドで進行中の壁打ちがあるか。 */
bool has_sparring_session(const std::string& thread_id);

/** そのスレッドでペーパーレビュー待ちか (準備中・メモリ・永続ドラフトのいずれか)。 */
bool has_paper_review(const std::string& thread_id);

/** このプロセスでそのスレッドの議論/改善が進行中か (C++ 版の追加。テストと Gateway の案内用)。 */
bool is_discussion_running(const std::string& thread_id);

/**
 * フォーラム投稿を新フローで起動する。
 * discussion/improvement/learning は完走させ結論を投稿 (レビューゲート時は草案を出して返る)。
 * sparring は session を登録して返信を待つ。失敗はスレッドに通知して握りつぶす。
 */
void start_forum_flow(StartForumFlowInput input, const FlowDiscordDeps& deps, const FlowLiveHooks& hooks = {});

/**
 * ペーパーレビュー中のスレッド返信を処理する (壁打ちより優先で呼ぶ)。
 * 承認語 → 確定ペーパーで議論を開始 / 「戻す」→ 1 手前 / それ以外 → 自然文の調整指示。
 * @returns レビュー返信として処理したら true。
 */
bool handle_paper_review_reply(const std::string& thread_id, const std::string& guild_id, const std::string& text,
                               const FlowDiscordDeps& deps, const FlowLiveHooks& hooks = {});

/** レビュー中ペーパーを承認して議論を開始する (テキスト「開始」/ ✅ 共通)。承認待ちが無ければ false。 */
bool handle_paper_review_approval(const std::string& thread_id, const FlowDiscordDeps& deps, const FlowLiveHooks& hooks = {},
                                  const std::optional<std::string>& guild_id = std::nullopt);

/** フロー再提案で「議論」「改善」を選び直したとき、今のペーパーのまま続ける。続けたら true。 */
bool continue_paper_review_with_flow(const std::string& thread_id, const FlowKind& flow, const FlowDiscordDeps& deps,
                                     const std::optional<std::string>& guild_id = std::nullopt);

/** レビュー待ちを破棄する (タイマー・メモリ・draft 行)。何かを破棄したら true。 */
bool cancel_paper_review(const std::string& thread_id);

/** 途中で止まった議論のスレッドで「再開」等と返信されたら、確定済みのペーパーでやり直す。処理したら true。 */
bool handle_interrupted_discussion_reply(const std::string& thread_id, const std::string& guild_id, const std::string& text,
                                         const FlowDiscordDeps& deps, const FlowLiveHooks& hooks = {});

/** 進行中の壁打ちスレッドへの返信を処理する。壁打ちとして処理したら true。 */
bool handle_forum_flow_reply(const std::string& thread_id, const std::string& guild_id, const std::string& text,
                             const FlowDiscordDeps& deps, const FlowLiveHooks& hooks = {});

/** レビュー承認の案内文 (タイムアウト設定があれば併記)。テスト用に公開。 */
std::string approval_guide();

} // namespace di::flow
