#pragma once
// discord_live.cpp を Praesidium の静的解析 (1 単位 60 秒) に収まるよう分けた各単位が共有する状態・補助関数の宣言。
#include "flow/discord_live.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include "base/executor.hpp"
#include "base/json.hpp"
#include "base/log.hpp"
#include "base/strings.hpp"
#include "config.hpp"
#include "discord_hook/poster.hpp"
#include "flow/cost_logger.hpp"
#include "flow/discord_flow_runs.hpp"
#include "flow/discussion_paper.hpp"
#include "flow/discussion_redo.hpp"
#include "flow/learning_autocrawl.hpp"
#include "flow/notion_link.hpp"
#include "flow/paper_markdown.hpp"
#include "flow/paper_reuse.hpp"
#include "flow/paper_review_enrich.hpp"
#include "flow/paper_review_intent.hpp"
#include "flow/paper_review_session.hpp"
#include "flow/paper_revisions.hpp"
#include "flow/paper_voices.hpp"
#include "flow/persona_display.hpp"
#include "flow/project_premise.hpp"
#include "flow/spec_analyze.hpp"
#include "flow/spec_source.hpp"
#include "flow/user_voices/collect.hpp"
#include "flow/user_voices/request_client.hpp"
#include "flow/vote_display.hpp"

namespace di::flow {
namespace dh = di::discord_hook;

namespace discord_live_detail {


/** レビュー待ちのディスカッションペーパー (threadId → 草案 + 起動入力)。スレッド返信で調整/承認する。 */
struct PendingPaperReview {
    StartForumFlowInput input;
    PaperDraft draft;
    PaperReviewInfo info;
    std::optional<FlowLiveHooks> hooks;
    /** 無操作の自動開始タイマー (timeoutMs>0 時のみ。承認/調整で張り直す)。g_mu で守る。 */
    std::optional<Scheduler::Id> timer;
};

using PendingPtr = std::shared_ptr<PendingPaperReview>;

inline 
// モジュール内の状態 (TS の module-level Map / Set)。
std::mutex g_mu;

/** 進行中の壁打ちセッション (threadId → session)。 */
inline std::map<std::string, std::shared_ptr<SparringSession>> g_sparring;

/** スレッドごとのユーザーの声の検索 (未設定は deps.listExternalVoices)。 */
inline std::map<std::string, ListExternalVoicesFn> g_voice_lookups;

inline std::map<std::string, PendingPtr> g_reviews;

/** このプロセスで進行中の議論/改善スレッド (やり直しの二重起動を防ぐ)。 */
inline std::set<std::string> g_running;

std::string error_message(const std::exception& e);

void cancel_timer_locked(PendingPaperReview& p);

PendingPtr find_review(const std::string& thread_id);

void set_review(const std::string& thread_id, PendingPtr p);

bool erase_review(const std::string& thread_id);

void clear_review_timer(const PendingPtr& p);

bool is_running(const std::string& thread_id);

std::optional<std::string> resolve_youtube_api_key(const FlowDiscordDeps& deps);

std::string workspace_of(const FlowDiscordDeps& deps);

ListExternalVoicesFn voice_lookup_for(const std::string& thread_id, const FlowDiscordDeps& deps);

void set_voice_lookup(const std::string& thread_id, ListExternalVoicesFn fn);

bool submit_background(const FlowDiscordDeps& deps, std::function<void()> task);

void post_thread_notice(const FlowDiscordDeps& deps, const std::string& thread_id, const std::string& content);

std::string flow_label(const FlowKind& flow);


/** 1 議論ぶんの投稿コンテキスト (utterance_id → 投稿 message_id / 露出名)。 */
struct ThreadPostCtx {
    std::mutex mu;
    std::map<std::string, std::string> messageIdByUtterance;
    std::map<std::string, std::string> displayNameByUtterance;
};

std::function<void(const FlowUtteranceRecord&)> make_thread_utterance_poster(const FlowDiscordDeps& deps, const std::string& thread_id,
                                                                             std::shared_ptr<ThreadPostCtx> ctx);

std::function<void(const VoteEvent&)> make_thread_vote_reactor(const FlowDiscordDeps& deps, const std::string& thread_id,
                                                               std::shared_ptr<ThreadPostCtx> ctx);

DispatchDeps build_dispatch_deps(const FlowDiscordDeps& deps, const std::string& thread_id);

DispatchResult run_dispatch(const FlowDiscordDeps& deps, const DispatchInput& input, const DispatchDeps& d);

std::string resolve_starter_notion(const StartForumFlowInput& input, const FlowDiscordDeps& deps);

void collect_user_voices_for_thread(const StartForumFlowInput& input, const FlowDiscordDeps& deps);

std::optional<ReusablePaper> resolve_reused_paper(const StartForumFlowInput& input, const FlowDiscordDeps& deps, bool& specified);

void save_thread_voices(const StartForumFlowInput& input, const FlowDiscordDeps& deps);

void persist_discord_draft(const StartForumFlowInput& input, const PaperDraft& draft);

PaperDraftBuild build_forum_paper_draft(const StartForumFlowInput& input, const FlowDiscordDeps& deps, const std::string& notion_md,
                                        const std::optional<ReusablePaper>& reused);

PaperIntentDeps build_intent_deps(const StartForumFlowInput& input, const FlowDiscordDeps& deps);

inline 
/** 無操作の自動開始タイマーを (張り直して) 仕掛ける。timeoutMs<=0 なら何もしない。 */
void schedule_review_auto_start(const std::string& thread_id, const FlowDiscordDeps& deps);

PaperOverride override_from_pending(const PendingPaperReview& p);

void record_forced_debatability(const std::string& thread_id, const PaperReviewInfo& info);

void run_discussion_dispatch_inner(const StartForumFlowInput& input, const FlowDiscordDeps& deps, const FlowLiveHooks& hooks,
                                   const std::optional<PaperOverride>& paper_override);

void run_discussion_dispatch_marked(const StartForumFlowInput& input, const FlowDiscordDeps& deps, const FlowLiveHooks& hooks,
                                    const std::optional<PaperOverride>& paper_override);

void mark_running(const std::string& thread_id);

void run_discussion_dispatch(const StartForumFlowInput& input, const FlowDiscordDeps& deps, const FlowLiveHooks& hooks,
                             const std::optional<PaperOverride>& paper_override = std::nullopt);

PendingPtr rehydrate_paper_review(const std::string& thread_id, const std::string& guild_id, const std::optional<FlowLiveHooks>& hooks);

void handle_paper_review_revert(const std::string& thread_id, const PendingPtr& pending, const FlowDiscordDeps& deps);

user_voices::VoiceRequestPort voice_request_of(const FlowDiscordDeps& deps);

void approve_pending(const std::string& thread_id, const PendingPtr& pending, const FlowDiscordDeps& deps,
                     const FlowLiveHooks& hooks, const std::string& notice);

bool process_paper_review_reply(const std::string& thread_id, const std::string& guild_id, const std::string& text,
                                const FlowDiscordDeps& deps, const FlowLiveHooks& hooks);

void schedule_review_auto_start(const std::string& thread_id, const FlowDiscordDeps& deps);

void start_paper_review(const StartForumFlowInput& input, const FlowDiscordDeps& deps, const FlowLiveHooks& hooks,
                        const std::string& notion_md, const std::optional<ReusablePaper>& reused);

void start_learning(const StartForumFlowInput& input, const FlowDiscordDeps& deps, const DispatchDeps& dispatch_deps,
                    const std::string& scene);

} // namespace discord_live_detail
} // namespace di::flow
