#include "flow/discord_live_internal.hpp"
#include "flow/transcript/discord_message.hpp"

// フォーラム議論/改善の実行と締め (結論の投稿・まとめ HTML・onConcluded)。ペーパー準備 (discord_live_paper.cpp) から分けた。
namespace di::flow {
namespace discord_live_detail {

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
        // まとめ + チャット風 HTML。onConcluded がスレッドを lock+archive するので、その前に送る。
        transcript::post_concluded_transcript(deps.api.get(), input.threadId, *d.sessionId, input.theme, flow_label(input.flow));
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

} // namespace discord_live_detail
} // namespace di::flow
