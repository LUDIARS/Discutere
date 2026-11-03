// src/slack/slack-live.ts の移植。
#include "slack/slack_live.hpp"

#include <map>
#include <memory>
#include <mutex>

#include "base/log.hpp"
#include "base/strings.hpp"
#include "flow/persona_display.hpp"
#include "flow/vote_display.hpp"

namespace di::slack {

namespace {

/** 進行中の壁打ちセッション (thread_ts → session)。 */
struct SparringRegistry {
    std::mutex m;
    std::map<std::string, std::shared_ptr<flow::SparringSession>> byThread;
};

SparringRegistry& sparring() {
    static SparringRegistry r;
    return r;
}

/** 1 議論ぶんの投稿コンテキスト (utterance_id → 投稿 ts / 露出名)。 */
struct ThreadPostCtx {
    std::mutex m;
    /** utterance_id → 投稿した Slack message ts (リアクション付与先)。 */
    std::map<std::string, std::string> tsByUtterance;
    /** utterance_id → 露出名 (得票集計の表示用)。 */
    std::map<std::string, std::string> displayNameByUtterance;
};

/** bot 名義 (username 無し) でスレッドに 1 メッセージ投稿する (結論・案内用)。失敗は握る。 */
void post_thread_notice(const SlackFlowDeps& deps, const std::string& channel, const std::string& thread_ts, const std::string& text) {
    try {
        PostMessageArgs a;
        a.channel = channel;
        a.threadTs = thread_ts;
        a.text = text;
        post_message(deps.botToken, a, deps.webApi);
    } catch (const std::exception& e) {
        log::warn("  slack-live: notice post 失敗 (thread=" + thread_ts + "): " + e.what());
    }
}

/**
 * スレッドへ persona 名で発話を流す onUtterance を作る。
 * username が効かない環境 (chat:write.customize 無し) に備え、本文先頭にも太字で表示名を出す。
 */
std::function<void(const flow::FlowUtteranceRecord&)> make_thread_utterance_poster(const SlackFlowDeps& deps, const std::string& channel,
                                                                                  const std::string& thread_ts, std::shared_ptr<ThreadPostCtx> ctx) {
    return [deps, channel, thread_ts, ctx](const flow::FlowUtteranceRecord& u) {
        flow::DisplayNameInput dn;
        dn.name = u.personaName;
        dn.stance = u.stance;
        dn.role = u.role;
        dn.possessionName = u.possessionName;
        const std::string display_name = flow::compose_display_name(dn);
        try {
            // username が無効でも誰の発話か分かるよう、本文先頭に表示名 (mrkdwn 太字) を付ける。
            PostMessageArgs a;
            a.channel = channel;
            a.threadTs = thread_ts;
            a.username = display_name;
            a.iconEmoji = ":speech_balloon:";
            a.text = "*" + display_name + "*\n" + u.text;
            const auto posted = post_message(deps.botToken, a, deps.webApi);
            std::lock_guard lock(ctx->m);
            if (!posted.ts.empty()) ctx->tsByUtterance[u.id] = posted.ts;
            ctx->displayNameByUtterance[u.id] = display_name;
        } catch (const std::exception& e) {
            log::warn("  slack-live: utterance post 失敗 (thread=" + thread_ts + "): " + e.what());
        }
    };
}

/**
 * ラウンド投票の可視化。世論 (winner) に :trophy:、得票のあった発話に :+1: を付け、
 * 得票集計テキストをスレッドに投稿する。得票 0 なら何もしない。
 */
std::function<void(const flow::VoteEvent&)> make_thread_vote_reactor(const SlackFlowDeps& deps, const std::string& channel,
                                                                    const std::string& thread_ts, std::shared_ptr<ThreadPostCtx> ctx) {
    return [deps, channel, thread_ts, ctx](const flow::VoteEvent& e) {
        flow::VoteDisplay vd{e.kind, e.tally, e.winner};
        if (!flow::has_visible_vote_result(vd)) return;  // 全員棄権 / winner なし → 可視化しない

        // 🏆/👍 を付与 (ts が取れている発話のみ)。
        // kind="scores" (improvement の機械スコア) は票数ではないため :trophy: のみ (:+1: の枚数積みはしない)。
        const bool stack_votes = flow::should_stack_vote_reactions(e.kind);
        for (const auto& [utterance_id, votes] : e.tally) {
            const bool is_winner = e.winner && utterance_id == *e.winner;
            if (!is_winner && (!stack_votes || votes <= 0)) continue;
            std::string ts;
            {
                std::lock_guard lock(ctx->m);
                const auto it = ctx->tsByUtterance.find(utterance_id);
                if (it == ctx->tsByUtterance.end()) continue;
                ts = it->second;
            }
            reactions_add(deps.botToken, ReactionsAddArgs{channel, ts, is_winner ? VOTE_WINNER_REACTION : VOTE_REACTION}, deps.webApi);
        }

        // 得票/スコア集計テキスト (降順。scores は「スコア x.xx (design_gap 適合)」表記)。
        const auto ranked = flow::build_vote_ranking_lines(vd, [&](const std::string& id) {
            std::lock_guard lock(ctx->m);
            const auto it = ctx->displayNameByUtterance.find(id);
            return it == ctx->displayNameByUtterance.end() ? std::string("(不明)") : it->second;
        });
        if (!ranked.empty())
            post_thread_notice(deps, channel, thread_ts, "*" + flow::vote_notice_title(e.round, e.kind) + "*\n" + str::join(ranked, "\n"));
    };
}

flow::DispatchDeps build_dispatch_deps(const SlackFlowDeps& deps, const std::string& channel, const std::string& thread_ts) {
    auto ctx = std::make_shared<ThreadPostCtx>();
    flow::DispatchDeps d;
    d.llm = deps.llm;
    d.listExternalVoices = deps.listExternalVoices;
    d.sentimentClients = deps.sentimentClients;
    d.gamesDir = deps.gamesDir;
    d.workspaceId = deps.workspaceId;
    d.onUtterance = make_thread_utterance_poster(deps, channel, thread_ts, ctx);
    d.onVote = make_thread_vote_reactor(deps, channel, thread_ts, ctx);
    d.log = [thread_ts](const std::string& m) { log::info("  [slack-live " + thread_ts + "] " + m); };
    d.warn = [thread_ts](const std::string& m) { log::warn("  [slack-live/warn " + thread_ts + "] " + m); };
    d.drivers = deps.drivers;
    return d;
}

} // namespace

void reset_slack_live_for_test() {
    std::lock_guard lock(sparring().m);
    sparring().byThread.clear();
}

bool has_slack_sparring_session(const std::string& thread_ts) {
    std::lock_guard lock(sparring().m);
    return sparring().byThread.count(thread_ts) > 0;
}

void start_slack_flow(const StartSlackFlowInput& input, const SlackFlowDeps& deps, const SlackFlowHooks& hooks) {
    const std::string scene = "slack:" + input.channel + "/" + input.threadTs;
    const std::string& channel = input.channel;
    const std::string& thread_ts = input.threadTs;
    try {
        const auto dispatch_deps = build_dispatch_deps(deps, channel, thread_ts);

        // ── 壁打ち: session を起動・登録し、以後のスレッド返信を submit_user に流す ──
        if (input.flow == "sparring") {
            flow::DispatchInput in;
            in.theme = input.theme;
            in.tags = input.tags;
            in.flow = input.flow;
            in.scene = scene;
            in.opponentPersonaIds = input.opponentPersonaIds;
            const auto result = flow::dispatch_flow(in, dispatch_deps);
            if (result.kind != "sparring" || !result.session) return;
            {
                std::lock_guard lock(sparring().m);
                sparring().byThread[thread_ts] = result.session;
            }
            post_thread_notice(deps, channel, thread_ts,
                               "🥊 *壁打ち* を始めます。あなたの主張・アイデアをこのスレッドに投稿してください。AI が反証・別視点で応答します。\n（`/終了` または十分話したら締めます）");
            return;
        }

        // ── 学習: 収集のみ。Core が要る ──
        if (input.flow == "learning") {
            if (!deps.openCore) {
                post_thread_notice(deps, channel, thread_ts, "⚠️ 学習フローは Core 未設定のため起動できません。");
                return;
            }
            post_thread_notice(deps, channel, thread_ts, "📚 *学習* (外部の声の収集) を開始します…");
            auto core = deps.openCore();
            struct Closer {
                core::Core* c;
                ~Closer() {
                    if (c) c->close();
                }
            } closer{core.get()};
            flow::DispatchInput in;
            in.theme = input.theme;
            in.tags = input.tags;
            in.flow = input.flow;
            in.scene = scene;
            auto d = dispatch_deps;
            d.core = core.get();
            const auto result = flow::dispatch_flow(in, d);
            if (result.kind == "learning" && result.learning)
                post_thread_notice(deps, channel, thread_ts,
                                   "✅ 学習完了: 「" + input.theme + "」を KG に取り込みました (slug=" + result.learning->gameSlug + ").");
            return;
        }

        // ── 議論 / 改善: 完走させて結論を投稿 ──
        std::string head = std::string("🗣️ *") + (input.flow == "improvement" ? "改善" : "議論") + "* を始めます: 「" + input.theme + "」";
        if (!input.tags.empty()) head += "\nタグ: " + str::join(input.tags, " / ");
        post_thread_notice(deps, channel, thread_ts, head);
        flow::DispatchInput in;
        in.theme = input.theme;
        in.tags = input.tags;
        in.flow = input.flow;
        in.scene = scene;
        in.rounds = input.rounds;
        in.turnsPerRound = input.turnsPerRound;
        const auto result = flow::dispatch_flow(in, dispatch_deps);
        if ((result.kind == "discussion" || result.kind == "improvement") && result.result) {
            const auto& r = *result.result;
            const std::string summary = r.concluded ? r.conclusion : "(明確な結論には至りませんでした)";
            post_thread_notice(deps, channel, thread_ts, "✅ *結論*\n" + summary);
            if (hooks.onConcluded) hooks.onConcluded(SlackConcluded{scene, input.theme, summary});
        }
    } catch (const std::exception& e) {
        log::warn("  slack-live: " + input.flow + " 起動失敗 (thread=" + thread_ts + "): " + e.what());
        post_thread_notice(deps, channel, thread_ts, std::string("⚠️ フロー実行中にエラーが発生しました: ") + e.what());
    }
}

bool handle_slack_flow_reply(const std::string& thread_ts, const std::string& channel, const std::string& text, const SlackFlowDeps& deps,
                             const SlackFlowHooks& hooks) {
    std::shared_ptr<flow::SparringSession> session;
    {
        std::lock_guard lock(sparring().m);
        const auto it = sparring().byThread.find(thread_ts);
        if (it == sparring().byThread.end()) return false;
        session = it->second;
    }
    try {
        const auto result = session->submit_user(text);
        if (result.kind == "ended") {
            {
                std::lock_guard lock(sparring().m);
                const auto it = sparring().byThread.find(thread_ts);
                if (it != sparring().byThread.end() && it->second == session) sparring().byThread.erase(it);
            }
            const std::string summary = result.concluded ? result.conclusion : "(壁打ちを終了しました)";
            post_thread_notice(deps, channel, thread_ts, "✅ *壁打ち終了*\n" + summary);
            if (hooks.onConcluded) hooks.onConcluded(SlackConcluded{"slack:" + channel + "/" + thread_ts, "壁打ち", summary});
        }
    } catch (const std::exception& e) {
        log::warn("  slack-live: 壁打ち返信処理失敗 (thread=" + thread_ts + "): " + e.what());
    }
    return true;
}

} // namespace di::slack
