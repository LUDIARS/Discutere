#include "flow/discord_live_internal.hpp"

namespace di::flow {
namespace dh = di::discord_hook;

namespace discord_live_detail {


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

} // namespace discord_live_detail
} // namespace di::flow
