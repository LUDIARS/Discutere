#include "flow/discord_live.hpp"
#include "flow/discord_live_internal.hpp"



namespace di::flow {
namespace dh = di::discord_hook;

namespace discord_live_detail {

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

} // namespace discord_live_detail

using namespace discord_live_detail;

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
