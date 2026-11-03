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

namespace {

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

// モジュール内の状態 (TS の module-level Map / Set)。
std::mutex g_mu;
/** 進行中の壁打ちセッション (threadId → session)。 */
std::map<std::string, std::shared_ptr<SparringSession>> g_sparring;
/** スレッドごとのユーザーの声の検索 (未設定は deps.listExternalVoices)。 */
std::map<std::string, ListExternalVoicesFn> g_voice_lookups;
std::map<std::string, PendingPtr> g_reviews;
/** このプロセスで進行中の議論/改善スレッド (やり直しの二重起動を防ぐ)。 */
std::set<std::string> g_running;

std::string error_message(const std::exception& e) { return e.what(); }

void cancel_timer_locked(PendingPaperReview& p) {
    if (p.timer) {
        global_scheduler().cancel(*p.timer);
        p.timer.reset();
    }
}

PendingPtr find_review(const std::string& thread_id) {
    std::lock_guard lock(g_mu);
    auto it = g_reviews.find(thread_id);
    return it == g_reviews.end() ? nullptr : it->second;
}

void set_review(const std::string& thread_id, PendingPtr p) {
    std::lock_guard lock(g_mu);
    g_reviews[thread_id] = std::move(p);
}

bool erase_review(const std::string& thread_id) {
    std::lock_guard lock(g_mu);
    auto it = g_reviews.find(thread_id);
    if (it == g_reviews.end()) return false;
    cancel_timer_locked(*it->second);
    g_reviews.erase(it);
    return true;
}

void clear_review_timer(const PendingPtr& p) {
    std::lock_guard lock(g_mu);
    cancel_timer_locked(*p);
}

bool is_running(const std::string& thread_id) {
    std::lock_guard lock(g_mu);
    return g_running.count(thread_id) > 0;
}

std::optional<std::string> resolve_youtube_api_key(const FlowDiscordDeps& deps) {
    if (deps.getYoutubeApiKey) return deps.getYoutubeApiKey();
    return deps.youtubeApiKey;
}

std::string workspace_of(const FlowDiscordDeps& deps) { return deps.workspaceId.value_or(config::get_config().workspace); }

ListExternalVoicesFn voice_lookup_for(const std::string& thread_id, const FlowDiscordDeps& deps) {
    {
        std::lock_guard lock(g_mu);
        auto it = g_voice_lookups.find(thread_id);
        if (it != g_voice_lookups.end()) return it->second;
    }
    return deps.listExternalVoices;
}

void set_voice_lookup(const std::string& thread_id, ListExternalVoicesFn fn) {
    std::lock_guard lock(g_mu);
    g_voice_lookups[thread_id] = std::move(fn);
}

bool submit_background(const FlowDiscordDeps& deps, std::function<void()> task) {
    if (deps.submitBackground) return deps.submitBackground(std::move(task));
    return background_executor().submit(std::move(task));
}

/** bot 名義でスレッドに 1 メッセージ投稿する (結論・案内用)。失敗は握り潰す。 */
void post_thread_notice(const FlowDiscordDeps& deps, const std::string& thread_id, const std::string& content) {
    try {
        if (!deps.api) throw std::runtime_error("discord api not configured");
        dh::post_discord_channel(*deps.api, dh::DiscordPostArgs{thread_id, content, std::nullopt});
    } catch (const std::exception& e) {
        log::warn("  flow-live: notice post 失敗 (thread=" + thread_id + "): " + error_message(e));
    }
}

std::string flow_label(const FlowKind& flow) {
    const auto& p = project_premises();
    return flow == "improvement" ? p.improvement.label : p.discussion.label;
}

/** 1 議論ぶんの投稿コンテキスト (utterance_id → 投稿 message_id / 露出名)。 */
struct ThreadPostCtx {
    std::mutex mu;
    std::map<std::string, std::string> messageIdByUtterance;
    std::map<std::string, std::string> displayNameByUtterance;
};

/** フォーラムスレッドへ persona 名で発話を流す onUtterance。露出名は「名前 (ロール/憑依ペルソナ)」。 */
std::function<void(const FlowUtteranceRecord&)> make_thread_utterance_poster(const FlowDiscordDeps& deps, const std::string& thread_id,
                                                                             std::shared_ptr<ThreadPostCtx> ctx) {
    auto api = deps.api;
    return [api, thread_id, ctx](const FlowUtteranceRecord& u) {
        const std::string display_name =
            compose_display_name(DisplayNameInput{u.personaName, FlowStance(u.stance), FlowRole(u.role), u.possessionName});
        try {
            if (!api) throw std::runtime_error("discord api not configured");
            const auto target = dh::resolve_webhook_target(*api, thread_id);
            const auto wh = dh::ensure_channel_webhook(*api, target.webhookChannelId);
            const std::string text = u.isError ? u.text : dh::humanize_for_discord(u.text);
            dh::DiscordWebhookPostArgs args;
            args.webhookId = wh.id;
            args.webhookToken = wh.token;
            args.username = display_name;
            args.content = text;
            args.threadId = target.threadId.value_or(thread_id);
            const std::string posted = dh::post_discord_webhook(*api, args);
            std::lock_guard lock(ctx->mu);
            if (!posted.empty()) ctx->messageIdByUtterance[u.id] = posted;
            ctx->displayNameByUtterance[u.id] = display_name;
        } catch (const std::exception& e) {
            log::warn("  flow-live: utterance post 失敗 (thread=" + thread_id + "): " + error_message(e));
        }
    };
}

/**
 * ラウンド投票の可視化 (item3)。bot は同一ユーザなので得票数ぶんのリアクションは付けられないため、
 * 世論 (winner) に 🏆、得票のあった発話に 👍 を付け、得票集計テキストをスレッドに投稿する。
 */
std::function<void(const VoteEvent&)> make_thread_vote_reactor(const FlowDiscordDeps& deps, const std::string& thread_id,
                                                               std::shared_ptr<ThreadPostCtx> ctx) {
    return [deps, thread_id, ctx](const VoteEvent& e) {
        const VoteDisplay view{e.kind, e.tally, e.winner};
        if (!has_visible_vote_result(view)) return;  // 全員棄権 / winner なし → 可視化しない
        // kind="scores" (improvement の機械スコア) は票数ではないため 🏆 のみ。
        const bool stack_votes = should_stack_vote_reactions(e.kind);
        for (const auto& [utterance_id, votes] : e.tally) {
            const bool is_winner = e.winner && utterance_id == *e.winner;
            if (!is_winner && (!stack_votes || votes <= 0)) continue;
            std::string message_id;
            {
                std::lock_guard lock(ctx->mu);
                auto it = ctx->messageIdByUtterance.find(utterance_id);
                if (it == ctx->messageIdByUtterance.end()) continue;
                message_id = it->second;
            }
            try {
                if (!deps.api) throw std::runtime_error("discord api not configured");
                dh::react_discord(*deps.api, thread_id, message_id, is_winner ? VOTE_WINNER_EMOJI : VOTE_EMOJI);
            } catch (const std::exception& ex) {
                log::warn("  flow-live: 投票リアクション失敗 (thread=" + thread_id + "): " + error_message(ex));
            }
        }
        const auto ranked = build_vote_ranking_lines(view, [&ctx](const std::string& id) {
            std::lock_guard lock(ctx->mu);
            auto it = ctx->displayNameByUtterance.find(id);
            return it == ctx->displayNameByUtterance.end() ? std::string("(不明)") : it->second;
        });
        if (!ranked.empty())
            post_thread_notice(deps, thread_id, "**" + vote_notice_title(e.round, e.kind) + "**\n" + str::join(ranked, "\n"));
    };
}

DispatchDeps build_dispatch_deps(const FlowDiscordDeps& deps, const std::string& thread_id) {
    auto ctx = std::make_shared<ThreadPostCtx>();
    DispatchDeps d;
    d.llm = deps.llm;
    d.listExternalVoices = voice_lookup_for(thread_id, deps);
    d.sentimentClients = deps.sentimentClients;
    d.gamesDir = deps.gamesDir;
    d.workspaceId = deps.workspaceId;
    d.onUtterance = make_thread_utterance_poster(deps, thread_id, ctx);
    d.onVote = make_thread_vote_reactor(deps, thread_id, ctx);
    d.log = [thread_id](const std::string& m) { log::info("  [flow-live " + thread_id + "] " + m); };
    d.warn = [thread_id](const std::string& m) { log::warn("  [flow-live/warn " + thread_id + "] " + m); };
    return d;
}

DispatchResult run_dispatch(const FlowDiscordDeps& deps, const DispatchInput& input, const DispatchDeps& d) {
    if (deps.dispatch) return deps.dispatch(input, d);
    return dispatch_flow(input, d);
}

/** 議題 (スレッド名) と starter 本文中の Notion リンクを Canalis でクロールして md にする (無ければ "")。 */
std::string resolve_starter_notion(const StartForumFlowInput& input, const FlowDiscordDeps& deps) {
    const std::string text = input.theme + "\n" + input.specText.value_or("");
    if (deps.expandNotionLinks) return deps.expandNotionLinks(text);
    ExpandNotionOpts opts;
    const std::string tid = input.threadId;
    opts.warn = [tid](const std::string& m) { log::warn("  [notion " + tid + "] " + m); };
    return expand_notion_links(text, opts);
}

/**
 * 議題のゲーム + 類似ゲームのユーザーの声を集め、スレッドの声の検索を均等混合に差し替える。
 * 結果はスレッドに一言で知らせる。失敗は議論を止めない。
 */
void collect_user_voices_for_thread(const StartForumFlowInput& input, const FlowDiscordDeps& deps) {
    // 機密/内部は Steam / Voluptas の声も集めない。
    if (!can_collect_external(input.tags)) return;
    const std::string tid = input.threadId;
    try {
        user_voices::PrepareUserVoicesArgs args;
        args.theme = input.theme;
        args.includeThemeGame = input.flow != "discussion";
        args.youtubeApiKey = resolve_youtube_api_key(deps);
        args.similarGames = input.similarGames.value_or(std::vector<std::string>{});
        args.openCore = deps.openCore;
        args.workspaceId = workspace_of(deps);
        args.baseLookup = deps.listExternalVoices;
        args.log = [tid](const std::string& m) { log::info("  [user-voices " + tid + "] " + m); };
        args.warn = [tid](const std::string& m) { log::warn("  [user-voices " + tid + "] " + m); };
        const auto prepared = deps.prepareUserVoices ? deps.prepareUserVoices(args) : user_voices::prepare_user_voices(args);
        if (prepared.lookup) set_voice_lookup(tid, prepared.lookup);
        if (!prepared.reports.empty()) {
            std::vector<std::string> lines;
            for (const auto& r : prepared.reports) lines.push_back("・" + user_voices::describe_voice_report(r));
            const std::string mixed = prepared.reports.size() > 1 ? "\n(ゲームごとに同じ件数ずつ混ぜて議論に使います)" : "";
            post_thread_notice(deps, tid, "🎮 ユーザーの声:\n" + str::join(lines, "\n") + mixed);
        }
    } catch (const std::exception& e) {
        log::warn("  [user-voices " + tid + "] 収集失敗 (既存の外部の声で継続): " + error_message(e));
    }
}

/** starter / 議題の「ペーパー#N」を解決し、スレッドに一言知らせる (指定が無ければ nullopt)。 */
std::optional<ReusablePaper> resolve_reused_paper(const StartForumFlowInput& input, const FlowDiscordDeps& deps, bool& specified) {
    specified = false;
    const auto no = parse_reuse_paper_no(input.theme + "\n" + input.specText.value_or(""));
    if (!no) return std::nullopt;
    specified = true;
    auto reused = load_reusable_paper(*no);
    post_thread_notice(deps, input.threadId, describe_reuse(*no, reused));
    return reused;
}

/** スレッドの声 (今の検索で引ける声) をペーパーに保存する。失敗は議論を止めない。 */
void save_thread_voices(const StartForumFlowInput& input, const FlowDiscordDeps& deps) {
    try {
        const auto lookup = voice_lookup_for(input.threadId, deps);
        if (lookup) save_paper_voices(forum_session_id(input.threadId), lookup({input.theme}, static_cast<int>(PAPER_VOICE_LIMIT)));
    } catch (const std::exception& e) {
        log::warn("  [paper-voices " + input.threadId + "] 声の保存失敗 (議論は続行): " + error_message(e));
    }
}

/** Discord レビュー草案を discussion_paper(status='draft') に永続/同期する (session_id=threadId)。 */
void persist_discord_draft(const StartForumFlowInput& input, const PaperDraft& draft) {
    NewPaper p;
    p.sessionId = forum_session_id(input.threadId);
    p.theme = draft.theme;
    p.tags = draft.tags;
    p.mechanics = draft.mechanics;
    p.supplement = draft.supplement;
    p.bodyMd = draft.bodyMd;
    persist_draft_paper(p, input.flow);
}

/**
 * フォーラム議論のペーパー草案を作る。Notion 資料と使い回すペーパーのゲーム内容があれば
 * 「ゲーム内容」(mechanicsContext) に載せ、使い回すペーパーのメカニクスを先頭に差す。
 */
PaperDraftBuild build_forum_paper_draft(const StartForumFlowInput& input, const FlowDiscordDeps& deps, const std::string& notion_md,
                                        const std::optional<ReusablePaper>& reused) {
    const auto& cfg = config::get_config();
    const auto& richness = cfg.flow.paperRichness;
    const auto mechanics_context = merge_reused_context(reused, notion_md.empty() ? std::nullopt : std::optional<std::string>(notion_md));
    const std::string tid = input.threadId;
    BuildPaperDraftDeps d;
    d.flow = input.flow;
    d.gamesDir = deps.gamesDir;
    if (auto lookup = voice_lookup_for(tid, deps)) d.listExternalVoices = lookup;
    d.llm = richness.enrichMechanics ? deps.llm.get() : nullptr;
    d.mechanicsTarget = static_cast<int>(richness.mechanicsTarget);
    if (!richness.enrichModel.empty()) d.enrichModel = richness.enrichModel;
    if (mechanics_context && !mechanics_context->empty()) {
        PaperFixedFields seed;
        seed.mechanicsContext = *mechanics_context;
        d.seed = seed;
    }
    if (reused && !reused->mechanics.empty()) d.extraMechanics = reused->mechanics;
    // 議論適性ゲート (09): 情報ゲートの後段・人間レビューの前 (無効 / 対象外フローは nullopt = 現行挙動)。
    // TS の information-gate-runner.resolveDebatabilityGate と同じ解決 (config + withCostLog)。
    std::optional<CostLoggedClient> gated;
    if (cfg.flow.debatability.enabled && (input.flow == "discussion" || input.flow == "improvement") && deps.llm) {
        CostLogContext ctx;
        ctx.flow = input.flow;
        ctx.sessionId = forum_session_id(tid);
        ctx.location = "debatability-gate";
        gated.emplace(*deps.llm, ctx);
        PaperDebatabilityGate gate;
        gate.llm = &*gated;
        gate.minArmableIssues = static_cast<int>(cfg.flow.debatability.minArmableIssues);
        d.debatability = gate;
    }
    d.warn = [tid](const std::string& m) { log::warn("  [paper-review " + tid + "] " + m); };
    return build_paper_draft(input.theme, input.tags, d);
}

/** ペーパー調整指示の材料集めに使う依存を組む。外部の声のクロールは autoCrawl の自動経路ソース横断。 */
PaperIntentDeps build_intent_deps(const StartForumFlowInput& input, const FlowDiscordDeps& deps) {
    const std::string tid = input.threadId;
    auto warn = [tid](const std::string& m) { log::warn("  [paper-intent " + tid + "] " + m); };
    // 機密/内部ではペーパー調整からも外部の声をクロール・参照しない。
    const auto open_core = can_collect_external(input.tags) ? deps.openCore : nullptr;
    PaperIntentDeps out;
    out.theme = input.theme;
    out.tags = input.tags;
    out.gamesDir = deps.gamesDir;
    {
        auto lookup = voice_lookup_for(tid, deps);
        auto gated = gate_external_voices<ListExternalVoicesFn>(input.tags, lookup ? std::optional<ListExternalVoicesFn>(lookup) : std::nullopt);
        if (gated) out.listExternalVoices = *gated;
    }
    out.llm = deps.llm.get();
    if (open_core) {
        const std::string theme = input.theme;
        const std::string workspace = workspace_of(deps);
        out.crawl = [open_core, deps, theme, workspace, tid, warn](const std::string& query) -> int {
            const auto& cfg = config::get_config().flow.autoCrawl;
            const auto youtube = resolve_youtube_api_key(deps);
            const auto sources = resolve_auto_crawl_sources(cfg.sources, youtube);
            auto core = open_core();
            int imported = 0;
            for (const auto& source : sources) {
                try {
                    CollectAndImportDeps c;
                    c.core = core.get();
                    c.theme = theme;
                    c.slug = derive_slug(theme);
                    c.workspaceId = workspace;
                    c.spec.source = source;
                    c.spec.query = query;
                    c.maxItems = static_cast<int>(std::min(cfg.maxItems, 100.0));
                    c.youtubeApiKey = youtube;
                    c.log = [tid](const std::string& m) { log::info("  [paper-intent " + tid + "] " + m); };
                    c.warn = warn;
                    imported += collect_and_import(c).imported;
                } catch (const std::exception& e) {
                    warn(source + " のクロール失敗: " + error_message(e));
                }
            }
            if (core) core->close();
            return imported;
        };
    }
    out.warn = warn;
    return out;
}

/** 無操作の自動開始タイマーを (張り直して) 仕掛ける。timeoutMs<=0 なら何もしない。 */
void schedule_review_auto_start(const std::string& thread_id, const FlowDiscordDeps& deps);

/** レビュー待ちの草案から dispatch へ渡す確定ペーパー (issues / 議論適性サマリ込み) を組む。 */
PaperOverride override_from_pending(const PendingPaperReview& p) {
    PaperOverride o;
    o.mechanics = p.draft.mechanics;
    o.supplement = p.draft.supplement;
    o.bodyMd = p.draft.bodyMd;
    // 承認済み論点 (09): ファシリテーター開幕プロンプトの参考として運ぶ。
    o.issues = p.draft.issues;
    const auto& d = p.info.debatability;
    if (d && !d->degraded) o.debatability = DebatabilitySummary{d->debatable, d->armableBothCount};
    return o;
}

/** 議論不適のまま強行したとき、評価結果を discussion_paper に記録する (09 監査ログ)。 */
void record_forced_debatability(const std::string& thread_id, const PaperReviewInfo& info) {
    const auto& d = info.debatability;
    if (!d || d->degraded || d->debatable) return;
    try {
        set_paper_debatability(forum_session_id(thread_id), to_json(*d));
    } catch (const std::exception& e) {
        log::warn("  [paper-review " + thread_id + "] debatability 記録失敗 (議論は続行): " + error_message(e));
    }
}

/** 本体: 確定ペーパー (任意) で議論/改善を完走させ、結論を投稿する。 */
void run_discussion_dispatch_inner(const StartForumFlowInput& input, const FlowDiscordDeps& deps, const FlowLiveHooks& hooks,
                                   const std::optional<PaperOverride>& paper_override) {
    const std::string scene = "discord:" + input.guildId + "/" + input.threadId;
    // 再起動でメモリ上の声の検索を失ったスレッド (やり直し等) は、保存した声で議論する。
    bool has_lookup = false;
    {
        std::lock_guard lock(g_mu);
        has_lookup = g_voice_lookups.count(input.threadId) > 0;
    }
    if (!has_lookup) {
        auto stored = load_paper_voices(forum_session_id(input.threadId));
        if (!stored.empty()) set_voice_lookup(input.threadId, stored_voice_lookup(std::move(stored)));
    }
    // 議論に使う声を保存し直す (レビュー中の取り込みで増えた声も含める)。
    save_thread_voices(input, deps);
    auto d = build_dispatch_deps(deps, input.threadId);
    // session_id=threadId: 編集ゲートの draft 行を 'started' に upsert する (重複行を作らない)。
    d.sessionId = forum_session_id(input.threadId);
    d.paperOverride = paper_override;
    DispatchInput in;
    in.theme = input.theme;
    in.tags = input.tags;
    in.flow = input.flow;
    in.scene = scene;
    in.rounds = input.rounds;
    in.turnsPerRound = input.turnsPerRound;
    const auto result = run_dispatch(deps, in, d);
    if ((result.kind == "discussion" || result.kind == "improvement") && result.result) {
        const auto& r = *result.result;
        const std::string summary = r.concluded ? r.conclusion : "(明確な結論には至りませんでした)";
        post_thread_notice(deps, input.threadId, "✅ **結論**\n" + summary);
        if (hooks.onConcluded) hooks.onConcluded(scene, input.theme, summary);
    }
}

/** 進行中の印を付けてから呼ぶ (呼び終わりに外す)。 */
void run_discussion_dispatch_marked(const StartForumFlowInput& input, const FlowDiscordDeps& deps, const FlowLiveHooks& hooks,
                                    const std::optional<PaperOverride>& paper_override) {
    try {
        run_discussion_dispatch_inner(input, deps, hooks, paper_override);
    } catch (...) {
        std::lock_guard lock(g_mu);
        g_running.erase(input.threadId);
        g_voice_lookups.erase(input.threadId);
        throw;
    }
    std::lock_guard lock(g_mu);
    g_running.erase(input.threadId);
    g_voice_lookups.erase(input.threadId);
}

void mark_running(const std::string& thread_id) {
    std::lock_guard lock(g_mu);
    g_running.insert(thread_id);
}

void run_discussion_dispatch(const StartForumFlowInput& input, const FlowDiscordDeps& deps, const FlowLiveHooks& hooks,
                             const std::optional<PaperOverride>& paper_override = std::nullopt) {
    mark_running(input.threadId);
    run_discussion_dispatch_marked(input, deps, hooks, paper_override);
}

/**
 * 永続ドラフト (status='draft') からレビュー待ちをメモリに復元する (再起動/プロセス跨ぎの再開)。
 * guildId は返信コンテキストから来た値を使う (draft 行に guildId は持たない)。無ければ nullptr。
 */
PendingPtr rehydrate_paper_review(const std::string& thread_id, const std::string& guild_id, const std::optional<FlowLiveHooks>& hooks) {
    // 承認直後は議論の開始記録 (draft→started) より先に返信が届くことがある。議論の実行中は復活させない。
    if (is_running(thread_id)) return nullptr;
    const auto row = get_draft_paper(forum_session_id(thread_id));
    if (!row) return nullptr;
    PaperContent fallback;
    fallback.theme = row->theme;
    fallback.tags = row->tags;
    fallback.supplement = row->supplement;
    fallback.mechanics = row->mechanics;
    auto pending = std::make_shared<PendingPaperReview>();
    pending->draft = with_derived_structure(row->bodyMd, fallback);
    pending->input.guildId = guild_id;
    pending->input.threadId = thread_id;
    pending->input.theme = row->theme;
    pending->input.flow = row->flow;
    pending->input.tags = row->tags;
    // 復元時は集めた情報サンプルは持たない (件数表示のみ最小)。
    pending->hooks = hooks;
    set_review(thread_id, pending);
    mark_reviewing(thread_id);
    return pending;
}

/** レビュー中ペーパーを 1 手前に戻す (Web の ↶戻すと対応)。 */
void handle_paper_review_revert(const std::string& thread_id, const PendingPtr& pending, const FlowDiscordDeps& deps) {
    const std::string sid = forum_session_id(thread_id);
    if (!can_revert(sid)) {
        post_thread_notice(deps, thread_id, "↶ これ以上戻せません。");
        return;
    }
    const auto reverted = revert_last(sid);
    if (!reverted) {
        post_thread_notice(deps, thread_id, "↶ これ以上戻せません。");
        return;
    }
    pending->draft = with_derived_structure(reverted->bodyMd, pending->draft);
    pending->input.theme = pending->draft.theme;
    pending->input.tags = pending->draft.tags;
    persist_discord_draft(pending->input, pending->draft);
    set_review(thread_id, pending);
    schedule_review_auto_start(thread_id, deps);
    post_thread_notice(deps, thread_id, "↶ " + (reverted->changeSummary.empty() ? std::string("1 手前に戻しました") : reverted->changeSummary));
    post_thread_notice(deps, thread_id, render_paper_review(pending->draft, pending->info));
}

user_voices::VoiceRequestPort voice_request_of(const FlowDiscordDeps& deps) {
    return deps.voiceRequest ? *deps.voiceRequest : user_voices::configured_voice_request();
}

/**
 * レビュー待ちを確定してセッションを閉じ、議論を始める (承認・自動開始 共通)。
 * 議論の完走は待たない: 直列キューを議論の間ふさぐと、確定後の返信 (再開など) が滞るため。
 */
void approve_pending(const std::string& thread_id, const PendingPtr& pending, const FlowDiscordDeps& deps,
                     const FlowLiveHooks& hooks, const std::string& notice) {
    const std::string sid = forum_session_id(thread_id);
    auto voice_state = user_voices::read_voice_preparation(sid);
    if (voice_state && voice_state->choice == "wait" && voice_state->requestStatus != "accepted") {
        post_thread_notice(deps, thread_id, "受付待ちを維持しています。「受付確認」または「外部の声なしで開始」と回答してください。");
        return;
    }
    if (voice_state) {
        voice_state->choice = "continue";
        user_voices::save_voice_preparation(sid, *voice_state);
    }
    erase_review(thread_id);
    close_review_session(thread_id);
    post_thread_notice(deps, thread_id, notice);
    record_forced_debatability(thread_id, pending->info);
    const FlowLiveHooks run_hooks = pending->hooks ? *pending->hooks : hooks;
    const StartForumFlowInput input = pending->input;
    const PaperOverride override_paper = override_from_pending(*pending);
    // TS は runDiscussionDispatch を同期に始めて runningDiscussions に入れる。C++ は積む前に印を付ける。
    mark_running(thread_id);
    const bool queued = submit_background(deps, [input, deps, run_hooks, override_paper, thread_id] {
        try {
            run_discussion_dispatch_marked(input, deps, run_hooks, override_paper);
        } catch (const std::exception& e) {
            log::warn("  flow-live: 議論開始失敗 (thread=" + thread_id + "): " + error_message(e));
        }
    });
    if (!queued) {
        // キュー満杯: 議論を始められない。レビュー待ちに戻して再度の「開始」を待つ (縮退)。
        {
            std::lock_guard lock(g_mu);
            g_running.erase(thread_id);
        }
        set_review(thread_id, pending);
        mark_reviewing(thread_id);
        log::warn("  flow-live: 背景キューが満杯のため議論を開始できない (thread=" + thread_id + ")");
        post_thread_notice(deps, thread_id, "⚠️ 混雑のため議論を開始できませんでした。少し待ってからもう一度 **「開始」** と回答してください。");
    }
}

/** レビュー返信 1 件の処理本体 (直列キューの中で呼ぶ)。 */
bool process_paper_review_reply(const std::string& thread_id, const std::string& guild_id, const std::string& text,
                                const FlowDiscordDeps& deps, const FlowLiveHooks& hooks) {
    // 準備中 (草案を出す前) の返信は捨てずに溜め、草案を出した後で処理する。
    if (buffer_reply(thread_id, text)) return true;
    // メモリに無くても永続ドラフト (status='draft') があれば再開する (再起動/別プロセス跨ぎ)。
    PendingPtr pending = find_review(thread_id);
    if (!pending) pending = rehydrate_paper_review(thread_id, guild_id, hooks);
    if (!pending) return false;
    const std::string trimmed = str::trim(text);
    if (trimmed.empty()) return true;  // 空返信は無視 (レビューは継続)

    try {
        const std::string voice_session = forum_session_id(thread_id);
        auto voice_state = user_voices::read_voice_preparation(voice_session);
        if (trimmed == "再議論") {
            post_thread_notice(deps, thread_id, "修正用ペーパーを確認中です。変更を回答し、よければ「開始」と回答してください。");
            return true;
        }
        if (trimmed == "受付を待つ") {
            clear_review_timer(pending);
            if (voice_state && voice_state->requestStatus == "not_needed") {
                post_thread_notice(deps, thread_id,
                                   "参考情報があり、Voluptas への依頼はありません。内容を確認し、よければ「開始」と回答してください。");
                return true;
            }
            if (voice_state) {
                voice_state->choice = "wait";
                user_voices::save_voice_preparation(voice_session, *voice_state);
            }
            post_thread_notice(deps, thread_id,
                               voice_state ? user_voices::voice_preparation_notice(*voice_state)
                                           : std::string("依頼情報がありません。外部の声なしで開始することもできます。"));
            return true;
        }
        if (trimmed == "受付確認") {
            voice_state = user_voices::check_voice_receipt(voice_session, voice_request_of(deps));
            post_thread_notice(deps, thread_id,
                               voice_state ? user_voices::voice_preparation_notice(*voice_state) : std::string("依頼情報がありません。"));
            return true;
        }
        if (trimmed == "外部の声なしで開始" || trimmed == "声なしで開始" || trimmed == "声なし開始") {
            if (voice_state) {
                voice_state->choice = "continue";
                user_voices::save_voice_preparation(voice_session, *voice_state);
            }
            approve_pending(thread_id, pending, deps, hooks, "外部の声なしで議論を始めます。事前知識と仮説を区別し、後から再議論できます。");
            return true;
        }
        if (is_approval_text(trimmed)) {
            approve_pending(thread_id, pending, deps, hooks, "✅ ペーパーを承認しました。議論を始めます…");
            return true;
        }
        // 戻す → 1 手前の本文に戻す (Web の ↶戻すと対応)。
        if (is_revert_text(trimmed)) {
            handle_paper_review_revert(thread_id, pending, deps);
            return true;
        }
        // 調整指示になっていない返信 (「ない」「はい」など) ではペーパーを書き換えない。
        if (is_non_instruction_reply(trimmed)) {
            log::info("  [paper-review " + thread_id + "] 調整指示ではない返信のためペーパーを更新しない: " +
                      json_stringify(Json(str::js_slice(trimmed, 0, 20))));
            return true;
        }
        // 調整 → 「議論内容の調整 / 外部の声の取り込み / 類似ゲーム」に振り分け、材料を足した指示を LLM で反映する。
        const auto intent = classify_paper_review_intent(trimmed);
        if (intent.kind != "adjust")
            post_thread_notice(deps, thread_id, "🔎 「" + paper_review_intent_label(intent.kind) + "」として処理します…");
        if (intent.kind == "similar_games" && !intent.terms.empty()) {
            // 類似ゲームの声も集めて、議題のゲームの声と均等に混ぜる。
            std::vector<std::string> merged = pending->input.similarGames.value_or(std::vector<std::string>{});
            for (const auto& t : intent.terms)
                if (std::find(merged.begin(), merged.end(), t) == merged.end()) merged.push_back(t);
            pending->input.similarGames = merged;
            collect_user_voices_for_thread(pending->input, deps);
        }
        const auto built = build_intent_instruction(intent, build_intent_deps(pending->input, deps));
        if (!built.notice.empty()) post_thread_notice(deps, thread_id, "🔎 " + built.notice);
        if (!built.instruction) return true;
        if (!deps.llm) throw std::runtime_error("LLM が未設定です");
        ApplyPaperEditOpts opts;
        const auto& model = config::get_config().flow.paperReview.model;
        if (!model.empty()) opts.model = model;
        opts.warn = [thread_id](const std::string& m) { log::warn("  [paper-review " + thread_id + "] " + m); };
        const auto edited = apply_paper_edit(pending->draft, *built.instruction, *deps.llm, opts);
        pending->draft = edited.draft;
        // 議題/タグの編集は dispatch 引数に反映する (override はメカニクス/観点補足のみ運ぶ)。
        pending->input.theme = edited.draft.theme;
        pending->input.tags = edited.draft.tags;
        set_review(thread_id, pending);
        schedule_review_auto_start(thread_id, deps);  // 調整があったら自動開始を延長

        post_thread_notice(deps, thread_id, "📝 " + edited.changeSummary);
        if (edited.applied) {
            // 版履歴に追記 (戻すの基点) + 一覧の下書き行を最新内容に同期。
            append_revision(AppendRevisionArgs{voice_session, edited.draft.bodyMd, edited.changeSummary, "llm-edit"});
            persist_discord_draft(pending->input, pending->draft);
            post_thread_notice(deps, thread_id, render_paper_review(pending->draft, pending->info));
            post_thread_notice(deps, thread_id,
                               "他に調整があれば回答してください。よければ **「開始」** と回答 (または ✅)、1 手戻すなら **「戻す」** と回答してください。");
        }
    } catch (const std::exception& e) {
        log::warn("  flow-live: ペーパーレビュー返信処理失敗 (thread=" + thread_id + "): " + error_message(e));
        post_thread_notice(deps, thread_id, "⚠️ 調整の反映に失敗しました: " + error_message(e));
    }
    return true;
}

void schedule_review_auto_start(const std::string& thread_id, const FlowDiscordDeps& deps) {
    const auto pending = find_review(thread_id);
    if (!pending) return;
    if (user_voices::needs_voice_choice(user_voices::read_voice_preparation(forum_session_id(thread_id)))) return;
    const double timeout_ms = config::get_config().flow.paperReview.timeoutMs;
    if (!(timeout_ms > 0)) return;
    std::lock_guard lock(g_mu);
    cancel_timer_locked(*pending);
    // タイマーのスレッドでは重い仕事をしない: 背景キューへ渡し、返信と同じ直列キューで自動開始する。
    pending->timer = global_scheduler().after(std::chrono::milliseconds(static_cast<long long>(timeout_ms)), [thread_id, deps] {
        const bool queued = submit_background(deps, [thread_id, deps] {
            try {
                run_serial(thread_id, [&] {
                    const auto p = find_review(thread_id);
                    if (!p) return;
                    approve_pending(thread_id, p, deps, p->hooks.value_or(FlowLiveHooks{}), "⏱️ 無操作のため草案のまま議論を始めます…");
                });
            } catch (const std::exception& e) {
                log::warn("  flow-live: ペーパー自動開始失敗 (thread=" + thread_id + "): " + error_message(e));
            }
        });
        if (!queued) log::warn("  flow-live: 背景キューが満杯のためペーパーの自動開始を見送る (thread=" + thread_id + ")");
    });
}

/**
 * ペーパー草案 (議題ブリーフ + 集めた情報) を作ってスレッドに出し、レビュー待ちに登録する。
 * 以後のスレッド返信は handle_paper_review_reply が「調整」または「承認」として処理する。
 */
void start_paper_review(const StartForumFlowInput& input, const FlowDiscordDeps& deps, const FlowLiveHooks& hooks,
                        const std::string& notion_md, const std::optional<ReusablePaper>& reused) {
    auto built = build_forum_paper_draft(input, deps, notion_md, reused);
    auto pending = std::make_shared<PendingPaperReview>();
    pending->input = input;
    pending->draft = built.draft;
    pending->info = built.info;
    pending->hooks = hooks;
    set_review(input.threadId, pending);
    const std::string sid = forum_session_id(input.threadId);
    // ドラフトを discussion_paper(status='draft') に永続 → 議論一覧に「下書き」として出す/再開できる。
    persist_discord_draft(input, pending->draft);
    save_thread_voices(input, deps);
    append_revision(AppendRevisionArgs{sid, pending->draft.bodyMd, "初期草案", "initial"});
    post_thread_notice(deps, input.threadId, render_paper_review(pending->draft, pending->info));
    bool prior_knowledge = false;
    for (const auto& m : pending->draft.mechanics)
        if (!m.source || *m.source != "llm") prior_knowledge = true;
    if (pending->draft.mechanicsContext && !str::trim(*pending->draft.mechanicsContext).empty()) prior_knowledge = true;
    user_voices::PrepareVoiceChoiceArgs va;
    va.sessionId = sid;
    va.theme = input.theme;
    va.referenceCount = pending->info.voiceCount;
    va.priorKnowledge = prior_knowledge;
    va.request = voice_request_of(deps);
    const auto voice_state = user_voices::prepare_voice_choice(va);
    post_thread_notice(deps, input.threadId, user_voices::voice_preparation_notice(voice_state));
    post_thread_notice(deps, input.threadId, approval_guide());
    // 議論不適 → フロー再提案 (09): 提案リプライ + 議論タイプ選択メニュー再提示 (hook 経由)。レビュー待ちは維持する。
    const auto& d = pending->info.debatability;
    if (d && !d->degraded && !d->debatable && d->recommendation && claim_repropose(input.threadId)) {
        post_thread_notice(deps, input.threadId, "論点を追記・修正するか、このまま **「開始」** で議論を開始できます。");
        try {
            if (hooks.onReproposeFlowType) hooks.onReproposeFlowType(input.guildId, input.threadId, input.theme, input.tags);
        } catch (const std::exception& e) {
            log::warn("  [paper-review " + input.threadId + "] タイプ選択メニュー再提示失敗: " + error_message(e));
        }
    }
    schedule_review_auto_start(input.threadId, deps);
    // 準備中に届いた返信を、草案を出した後で届いた順に処理する (返信を捨てない)。
    run_serial(input.threadId, [&] {
        for (const auto& text : mark_reviewing(input.threadId)) {
            // 溜まっていた「開始」で確定したら、残りはレビューとして扱わない。
            if (!has_review_session(input.threadId)) break;
            process_paper_review_reply(input.threadId, input.guildId, text, deps, hooks);
        }
    });
}

void start_learning(const StartForumFlowInput& input, const FlowDiscordDeps& deps, const DispatchDeps& dispatch_deps,
                    const std::string& scene) {
    if (!deps.openCore) {
        post_thread_notice(deps, input.threadId, "⚠️ 学習フローは Core 未設定のため起動できません。");
        return;
    }
    post_thread_notice(deps, input.threadId, "📚 **学習** (外部の声の収集) を開始します…");
    // 自動収集モード: config.flow.autoCrawl の自動経路ソース横断でテーマをクロール → KG 取込。
    const auto& crawl_cfg = config::get_config().flow.autoCrawl;
    const auto youtube = resolve_youtube_api_key(deps);
    const auto crawl_sources = crawl_cfg.enabled ? resolve_auto_crawl_sources(crawl_cfg.sources, youtube) : std::vector<AutoCrawlSource>{};
    std::optional<LearningCrawlSpec> learning_crawl;
    if (!crawl_sources.empty()) {
        LearningCrawlSpec spec;
        spec.sources = crawl_sources;
        spec.maxItems = static_cast<int>(crawl_cfg.maxItems);
        spec.youtubeApiKey = youtube;
        learning_crawl = spec;
    }
    // 仕様書解析 (②/③): starter 本文 + テキスト系添付ファイルをまとめて LLM 解析 → mechanics として記録。
    // 添付は URL 取得 (ローカルパス読みは不許可)。
    std::vector<std::string> spec_parts;
    if (input.specText && !input.specText->empty()) spec_parts.push_back(*input.specText);
    const std::string notion_md = resolve_starter_notion(input, deps);
    if (!notion_md.empty()) spec_parts.push_back(notion_md);
    for (const auto& url : input.specAttachmentUrls.value_or(std::vector<std::string>{})) {
        try {
            ResolveSpecOpts opts;
            opts.allowLocalPath = false;
            spec_parts.push_back(resolve_spec_text(url, opts));
        } catch (const std::exception& e) {
            log::warn("  [spec-analyze " + input.threadId + "] 添付取得失敗 (" + url + "): " + error_message(e));
        }
    }
    std::vector<GameMechanicEntry> mechanics;
    bool has_mechanics = false;
    if (!spec_parts.empty() && deps.llm) {
        AnalyzeSpecMechanicsArgs a;
        a.theme = input.theme;
        a.specText = str::join(spec_parts, "\n\n");
        const std::string tid = input.threadId;
        a.warn = [tid](const std::string& m) { log::warn("  [spec-analyze " + tid + "] " + m); };
        mechanics = analyze_spec_mechanics(*deps.llm, a);
        has_mechanics = true;
    }
    auto core = deps.openCore();
    struct CloseCore {
        std::unique_ptr<core::Core>& c;
        ~CloseCore() {
            if (c) c->close();
        }
    } closer{core};
    DispatchDeps d = dispatch_deps;
    d.core = core.get();
    d.learningCrawl = learning_crawl;
    if (has_mechanics) d.mechanics = mechanics;
    DispatchInput in;
    in.theme = input.theme;
    in.tags = input.tags;
    in.flow = input.flow;
    in.scene = scene;
    const auto result = run_dispatch(deps, in, d);
    if (result.kind == "learning" && result.learning) {
        const auto& r = *result.learning;
        const int collected = r.opinionsRecorded + r.crawledImported + r.mechanicsRecorded;
        std::vector<std::string> parts;
        if (r.mechanicsRecorded > 0) parts.push_back("仕様書メカニクス " + std::to_string(r.mechanicsRecorded) + " 件");
        if (r.crawledImported > 0) {
            std::vector<std::string> keys;
            if (r.crawledBySource.is_object())
                for (const auto& [k, v] : r.crawledBySource.items()) keys.push_back(k);
            parts.push_back("自動収集 " + std::to_string(r.crawledImported) + " 件 (" + str::join(keys, "/") + ")");
        }
        const std::string detail = parts.empty() ? "" : " (" + str::join(parts, " / ") + ")";
        post_thread_notice(deps, input.threadId,
                           collected > 0 ? "✅ 学習完了: 「" + input.theme + "」を KG に " + std::to_string(collected) + " 件取り込みました" +
                                               detail + " (slug=" + r.gameSlug + ")."
                                         : "✅ 学習完了: 「" + input.theme + "」(取込 0 件。収集元が見つからないか既出です。slug=" +
                                               r.gameSlug + ").");
    }
}

} // namespace

std::string approval_guide() {
    const double timeout_ms = config::get_config().flow.paperReview.timeoutMs;
    const std::string base =
        "✏️ 調整があればこのスレッドで回答してください。回答の内容で次の 3 つに振り分けて処理します。\n"
        "・議論内容の調整 (例:「メカニクスにガチャを追加」「観点補足を初心者向けに」)\n"
        "・外部の声の取り込み (例:「外部の声を取り込んで「周回」」— 指定語 (無ければ議題) で集めて根拠に反映)\n"
        "・類似するゲーム / メカニクスの代替 (例:「類似ゲーム: 「モンスト」「パズドラ」」— 挙げたゲームのメカニクスを参考として追加)\n"
        "よければ **「開始」** と回答するか ✅ を付けると議論を始めます。";
    if (timeout_ms > 0) {
        // Math.round (0.5 は正の無限大方向)
        const long long min = static_cast<long long>(std::floor(timeout_ms / 60000.0 + 0.5));
        const long long sec = static_cast<long long>(std::floor(timeout_ms / 1000.0 + 0.5));
        const std::string span = min > 0 ? std::to_string(min) + " 分" : std::to_string(sec) + " 秒";
        return base + "\n(" + span + "無操作なら草案のまま自動で始めます。ただし外部の声の選択待ち・受付待ちでは自動開始しません)";
    }
    return base;
}

void reset_flow_live_for_test() {
    {
        std::lock_guard lock(g_mu);
        g_sparring.clear();
        for (auto& [k, p] : g_reviews) cancel_timer_locked(*p);
        g_reviews.clear();
        g_voice_lookups.clear();
        g_running.clear();
    }
    reset_review_sessions_for_test();
}

bool has_sparring_session(const std::string& thread_id) {
    std::lock_guard lock(g_mu);
    return g_sparring.count(thread_id) > 0;
}

bool has_paper_review(const std::string& thread_id) {
    // 準備中 (草案を出す前) もレビューセッションに含める: その間の返信も受け付けて溜める。
    if (has_review_session(thread_id)) return true;
    {
        std::lock_guard lock(g_mu);
        if (g_reviews.count(thread_id)) return true;
    }
    return get_draft_paper(forum_session_id(thread_id)).has_value();
}

bool is_discussion_running(const std::string& thread_id) { return is_running(thread_id); }

void start_forum_flow(StartForumFlowInput input, const FlowDiscordDeps& deps, const FlowLiveHooks& hooks) {
    const std::string scene = "discord:" + input.guildId + "/" + input.threadId;
    try {
        if (!deps.api) throw std::runtime_error("discord api not configured");
        if (!deps.llm) throw std::runtime_error("LLM が未設定です");
        const DispatchDeps dispatch_deps = build_dispatch_deps(deps, input.threadId);

        // ── 壁打ち: session を起動・登録し、以後のスレッド返信を submit_user に流す ──
        if (input.flow == "sparring") {
            DispatchInput in;
            in.theme = input.theme;
            in.tags = input.tags;
            in.flow = input.flow;
            in.scene = scene;
            in.opponentPersonaIds = input.opponentPersonaIds;
            const auto result = run_dispatch(deps, in, dispatch_deps);
            if (result.kind != "sparring" || !result.session) return;
            {
                std::lock_guard lock(g_mu);
                g_sparring[input.threadId] = result.session;
            }
            post_thread_notice(deps, input.threadId,
                               "🥊 **壁打ち**を始めます。あなたの主張・アイデアを投稿してください。AI が反証・別視点で応答します。\n（`/終了` "
                               "または十分話したら締めます）");
            return;
        }

        // ── 学習: 収集のみ。Core が要る ──
        if (input.flow == "learning") {
            start_learning(input, deps, dispatch_deps, scene);
            return;
        }

        // ── 議論 / 改善 ──
        const std::string label = flow_label(input.flow);
        const bool review_enabled = config::get_config().flow.paperReview.enabled;
        // ペーパー確定までは 1 スレッド = 1 セッション。準備・レビュー中のスレッドで準備を二重に始めない。
        if (review_enabled && (has_paper_review(input.threadId) || !open_preparing(input.threadId))) {
            log::warn("  flow-live: ペーパー確認中のため準備を重ねて始めない (thread=" + input.threadId + ")");
            return;
        }
        std::string tags_line;
        if (!input.tags.empty()) tags_line = "\nタグ: " + str::join(input.tags, " / ");
        post_thread_notice(deps, input.threadId,
                           "🗣️ **" + label + "**の" + (review_enabled ? "準備をしています" : "を始めます") + ": 「" + input.theme + "」" +
                               tags_line);
        // ユーザーの声: 情報ゲートより先に集める。
        if (!input.similarGames)
            input.similarGames = user_voices::parse_similar_games(input.specText ? input.specText : std::optional<std::string>(input.theme));
        // ペーパーの使い回し (「ペーパー#12」): 声を引き継げたら集め直さない (paper-reuse.md)。
        bool reuse_specified = false;
        const auto reused = resolve_reused_paper(input, deps, reuse_specified);
        if (reused && !reused->voices.empty()) {
            set_voice_lookup(input.threadId, stored_voice_lookup(reused->voices));
        } else {
            collect_user_voices_for_thread(input, deps);
        }
        // starter / 議題中の Notion リンクはペーパーの「ゲーム内容」材料として展開する。
        const std::string notion_md = resolve_starter_notion(input, deps);
        if (!notion_md.empty()) post_thread_notice(deps, input.threadId, "📎 Notion リンクを読み込み、ペーパーの材料に加えました。");

        // ペーパーレビューゲート (有効時 / 声が引けないとき): 草案 + 集めた情報を出し、調整/承認をスレッド返信で待つ。
        const auto lookup = voice_lookup_for(input.threadId, deps);
        if (review_enabled || !lookup || lookup({input.theme}, 1).empty()) {
            start_paper_review(input, deps, hooks, notion_md, reused);
            return;
        }

        // 通常: そのまま完走させて結論を投稿 (Notion 資料・使い回すペーパーがあればそれを載せた草案で確定させる)。
        if (!notion_md.empty() || reused) {
            const auto built = build_forum_paper_draft(input, deps, notion_md, reused);
            PaperOverride o;
            o.mechanics = built.draft.mechanics;
            o.supplement = built.draft.supplement;
            o.bodyMd = built.draft.bodyMd;
            run_discussion_dispatch(input, deps, hooks, o);
            return;
        }
        run_discussion_dispatch(input, deps, hooks);
    } catch (const std::exception& e) {
        // 準備中に失敗したらセッションを閉じる (残すと以後の返信が溜まり続ける)。
        if (is_preparing(input.threadId)) close_review_session(input.threadId);
        log::warn("  flow-live: " + input.flow + " 起動失敗 (thread=" + input.threadId + "): " + error_message(e));
        post_thread_notice(deps, input.threadId, "⚠️ フロー実行中にエラーが発生しました: " + error_message(e));
    }
}

bool handle_paper_review_reply(const std::string& thread_id, const std::string& guild_id, const std::string& text,
                               const FlowDiscordDeps& deps, const FlowLiveHooks& hooks) {
    // 同じスレッドの返信は 1 件ずつ処理する (同時処理でペーパーが重複投稿されるのを防ぐ)。
    return run_serial(thread_id, [&] { return process_paper_review_reply(thread_id, guild_id, text, deps, hooks); });
}

bool handle_paper_review_approval(const std::string& thread_id, const FlowDiscordDeps& deps, const FlowLiveHooks& hooks,
                                  const std::optional<std::string>& guild_id) {
    return run_serial(thread_id, [&] {
        if (is_preparing(thread_id)) return false;  // 草案を出す前は承認できない
        PendingPtr pending = find_review(thread_id);
        if (!pending) pending = rehydrate_paper_review(thread_id, guild_id.value_or("dm"), hooks);
        if (!pending) return false;
        approve_pending(thread_id, pending, deps, hooks, "✅ ペーパーを承認しました。議論を始めます…");
        return true;
    });
}

bool continue_paper_review_with_flow(const std::string& thread_id, const FlowKind& flow, const FlowDiscordDeps& deps,
                                     const std::optional<std::string>& guild_id) {
    if (flow != "discussion" && flow != "improvement") return false;
    return run_serial(thread_id, [&] {
        if (is_preparing(thread_id)) {
            post_thread_notice(deps, thread_id, "⏳ ペーパーを準備中です。出来上がったらこのスレッドに投稿します。");
            return true;
        }
        PendingPtr pending = find_review(thread_id);
        if (!pending) pending = rehydrate_paper_review(thread_id, guild_id.value_or("dm"), std::nullopt);
        if (!pending) return false;
        pending->input.flow = flow;
        persist_discord_draft(pending->input, pending->draft);
        post_thread_notice(deps, thread_id,
                           "👌 今のペーパーのまま「" + flow_label(flow) +
                               "」で続けます。調整があれば回答、よければ **「開始」** と回答 (または ✅) してください。");
        return true;
    });
}

bool cancel_paper_review(const std::string& thread_id) {
    const bool had_memory = erase_review(thread_id);
    const bool had_session = has_review_session(thread_id);
    close_review_session(thread_id);
    bool had_draft = false;
    try {
        // draft 状態の行だけ消す (started 済みの議論データは触らない)。
        if (get_draft_paper(forum_session_id(thread_id))) had_draft = delete_flow_session(forum_session_id(thread_id));
    } catch (const std::exception& e) {
        log::warn("  [paper-review " + thread_id + "] レビュー破棄で draft 削除失敗: " + error_message(e));
    }
    return had_memory || had_draft || had_session;
}

bool handle_interrupted_discussion_reply(const std::string& thread_id, const std::string& guild_id, const std::string& text,
                                         const FlowDiscordDeps& deps, const FlowLiveHooks& hooks) {
    if (!is_redo_text(text)) return false;
    if (str::starts_with(str::trim(text), "再議論")) {
        return run_serial(thread_id, [&] {
            if (is_running(thread_id)) {
                post_thread_notice(deps, thread_id, "⏳ この議論は進行中です。終了後に再議論できます。");
                return true;
            }
            const auto session_id = open_rediscussion(thread_id);
            if (!session_id) return false;
            PendingPtr pending = find_review(thread_id);
            if (!pending) pending = rehydrate_paper_review(thread_id, guild_id, hooks);
            if (!pending) return false;
            clear_review_timer(pending);
            post_thread_notice(deps, thread_id,
                               "🔁 前回の記録を残して、再議論の下書きを開きました。意見・内容・参考情報の修正を回答してください。確認後に「開始」で再議論します。外部の声はなくても開始できます。");
            post_thread_notice(deps, thread_id, render_paper_review(pending->draft, pending->info));
            return true;
        });
    }
    const std::string sid = forum_session_id(thread_id);
    const auto paper = get_paper_snapshot(sid);
    RedoInput ri;
    if (paper) ri.paper = RedoPaper{paper->status, paper->flow};
    ri.concluded = paper ? has_flow_conclusion(sid) : false;
    ri.running = is_running(thread_id);
    const auto decision = decide_redo(ri);
    if (!decision.ok) {
        if (decision.reason == "running") {
            post_thread_notice(deps, thread_id, "⏳ この議論は進行中です。終わるまでお待ちください。");
            return true;
        }
        return false;
    }
    PaperContent fallback;
    fallback.theme = paper->theme;
    fallback.tags = paper->tags;
    fallback.supplement = paper->supplement;
    fallback.mechanics = paper->mechanics;
    const auto draft = with_derived_structure(strip_progress(paper->bodyMd), fallback);
    StartForumFlowInput input;
    input.guildId = guild_id;
    input.threadId = thread_id;
    input.theme = paper->theme;
    input.flow = paper->flow;
    input.tags = paper->tags;
    post_thread_notice(deps, thread_id, "🔁 途中で止まった議論を、確定済みのペーパーで最初からやり直します…\n(ラウンド数・ターン数は既定値です)");
    try {
        PaperOverride o;
        o.mechanics = draft.mechanics;
        o.supplement = draft.supplement;
        o.bodyMd = draft.bodyMd;
        o.issues = draft.issues;
        run_discussion_dispatch(input, deps, hooks, o);
    } catch (const std::exception& e) {
        log::warn("  flow-live: 議論のやり直し失敗 (thread=" + thread_id + "): " + error_message(e));
        post_thread_notice(deps, thread_id, "⚠️ 議論のやり直しに失敗しました: " + error_message(e));
    }
    return true;
}

bool handle_forum_flow_reply(const std::string& thread_id, const std::string& guild_id, const std::string& text,
                             const FlowDiscordDeps& deps, const FlowLiveHooks& hooks) {
    std::shared_ptr<SparringSession> session;
    {
        std::lock_guard lock(g_mu);
        auto it = g_sparring.find(thread_id);
        if (it == g_sparring.end()) return false;
        session = it->second;
    }
    try {
        const auto result = session->submit_user(text);
        if (result.kind == "ended") {
            {
                std::lock_guard lock(g_mu);
                g_sparring.erase(thread_id);
            }
            const std::string summary = result.concluded ? result.conclusion : "(壁打ちを終了しました)";
            post_thread_notice(deps, thread_id, "✅ **壁打ち終了**\n" + summary);
            if (hooks.onConcluded) hooks.onConcluded("discord:" + guild_id + "/" + thread_id, "壁打ち", summary);
        }
    } catch (const std::exception& e) {
        log::warn("  flow-live: 壁打ち返信処理失敗 (thread=" + thread_id + "): " + error_message(e));
    }
    return true;
}

} // namespace di::flow
