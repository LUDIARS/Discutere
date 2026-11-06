#include "flow/discord_live_internal.hpp"

namespace di::flow {
namespace dh = di::discord_hook;

namespace discord_live_detail {


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
                             const std::optional<PaperOverride>& paper_override) {
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

} // namespace discord_live_detail
} // namespace di::flow
