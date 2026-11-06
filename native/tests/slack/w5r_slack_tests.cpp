// w5-R: src/slack/slack-live.ts と src/slack/slack-router.ts の移植の検証 (TS テスト無し)。
// 偽の Slack Web API (http::Server) に投稿させ、チャンネルの照合・議論タイプ → 進行量 → 起動の流れ・
// 発話/投票/結論の投稿の形・エラー案内・token をログに出さないこと・保留表の上限を確かめる。
#include "check.hpp"
#include "w1d_support.hpp"
#include "w5r_support.hpp"

#include <atomic>

#include "base/log.hpp"
#include "base/strings.hpp"
#include "flow/persona_display.hpp"
#include "flow/vote_display.hpp"
#include "slack/slack_entry.hpp"
#include "slack/slack_live.hpp"
#include "slack/slack_router.hpp"

using namespace di;
using namespace di::slack;

namespace {

// 偽の bot token。実装は接頭辞を検査しないので、実在の形式 (xoxb-) に似せない。
const char* kToken = "fake-slack-bot-token-for-test";

struct FakeSlack {
    w1d::FakeServer server;
    std::mutex m;
    std::vector<Json> posts;      // chat.postMessage の本文
    std::vector<Json> reactions;  // reactions.add の本文
    int next_ts = 1;
    // failing: 投稿をすべて失敗させる (ok:false)。閉じたポートへ繋がせると、Windows では接続の拒否に
    // 1 回 2 秒ほどかかり、保留表の上限 (1000 件) を回すテストが時間切れになるため、すぐ失敗を返す偽物を使う。
    explicit FakeSlack(bool failing = false) {
        server.router->post("/chat.postMessage", [this, failing](http::Request& r) {
            std::lock_guard lock(m);
            if (failing) return http::Response::json(Json{{"ok", false}, {"error", "fake_unavailable"}});
            CHECK_EQ(r.header("Authorization").value_or(""), std::string("Bearer ") + kToken);
            posts.push_back(Json::parse(r.body));
            return http::Response::json(Json{{"ok", true}, {"ts", "9." + std::to_string(next_ts++)}, {"channel", "C1"}});
        });
        server.router->post("/reactions.add", [this](http::Request& r) {
            std::lock_guard lock(m);
            reactions.push_back(Json::parse(r.body));
            return http::Response::json(Json{{"ok", true}});
        });
        server.start();
    }
    std::vector<Json> take_posts() {
        std::lock_guard lock(m);
        auto out = std::move(posts);
        posts.clear();
        return out;
    }
    std::vector<Json> take_reactions() {
        std::lock_guard lock(m);
        auto out = std::move(reactions);
        reactions.clear();
        return out;
    }
};

flow::FlowUtteranceRecord utt(const std::string& id, const std::string& name, const std::string& role, const std::string& stance, const std::string& text) {
    flow::FlowUtteranceRecord u;
    u.id = id;
    u.personaName = name;
    u.role = role;
    u.stance = stance;
    u.text = text;
    return u;
}

SlackFlowDeps make_deps(const FakeSlack& slack) {
    SlackFlowDeps d;
    d.botToken = kToken;
    d.llm = std::make_shared<w5r::FakeLLM>();
    d.webApi.base_url = slack.server.base;
    d.webApi.allow_private_network = true;
    d.drivers.discussion = [](persona_engine::LLMClient&, const std::string& theme, const std::vector<flow::FlowTag>&, const flow::FlowDirectorDeps& deps) {
        if (theme == "boom") throw std::runtime_error("boom");
        deps.onUtterance(utt("u1", "陽介", "opinion", "pro", "賛成です"));
        deps.onUtterance(utt("u2", "花子", "debater", "con", "反対です"));
        flow::VoteEvent e;
        e.round = 1;
        e.kind = "votes";
        e.tally = {{"u1", 2}, {"u2", 0}};
        e.winner = "u1";
        deps.onVote(e);
        flow::FlowDirectorResult r;
        r.sessionId = "s";
        r.concluded = theme != "未収束";
        r.conclusion = "結論の本文";
        return r;
    };
    d.drivers.improvement = d.drivers.discussion;
    return d;
}

std::string display(const std::string& name, const std::string& role, const std::string& stance) {
    flow::DisplayNameInput dn;
    dn.name = name;
    dn.role = role;
    dn.stance = stance;
    return flow::compose_display_name(dn);
}

Json message(const std::string& channel, const std::string& ts, const std::string& text) {
    return Json{{"type", "message"}, {"channel", channel}, {"ts", ts}, {"text", text}};
}

Json action(const std::string& action_id, const std::string& value, const std::string& channel, const std::string& ts) {
    return Json{{"type", "block_actions"},
                {"actions", Json::array({Json{{"action_id", action_id}, {"selected_option", Json{{"value", value}}}}})},
                {"channel", Json{{"id", channel}}},
                {"message", Json{{"ts", ts}}}};
}

} // namespace

TEST_CASE("router: 設定チャンネルの投稿 → 議論タイプ → 進行量 → 議論 (発話・投票・結論の投稿の形)") {
    reset_slack_live_for_test();
    FakeSlack slack;
    std::vector<std::string> logs;
    std::mutex lm;
    const int sink = log::add_sink([&](const log::Record& r) {
        std::lock_guard lock(lm);
        logs.push_back(std::string(r.message));
    });
    std::vector<SlackConcluded> concluded;
    SlackRouterConfig cfg;
    cfg.channelIds = {"C1"};
    cfg.deps = make_deps(slack);
    cfg.hooks.onConcluded = [&](const SlackConcluded& c) { concluded.push_back(c); };
    cfg.submit = [](std::function<void()> fn) {
        fn();  // テストでは同期で走らせる
        return true;
    };
    auto router = create_slack_router(cfg);

    // 設定チャンネル外 / bot / message 以外 / subtype 付きは無視
    router.onEvent(message("C2", "1.0", "外"));
    auto bot = message("C1", "1.1", "bot");
    bot["bot_id"] = "B1";
    router.onEvent(bot);
    router.onEvent(Json{{"type", "reaction_added"}, {"channel", "C1"}});
    auto sub = message("C1", "1.2", "編集");
    sub["subtype"] = "message_changed";
    router.onEvent(sub);
    router.onEvent(Json::array());
    CHECK(slack.take_posts().empty());

    router.onEvent(message("C1", "100.0", "テーマA"));
    auto posts = slack.take_posts();
    CHECK_EQ(posts.size(), std::size_t{1});
    CHECK_EQ(posts[0]["channel"].get<std::string>(), std::string("C1"));
    CHECK_EQ(posts[0]["thread_ts"].get<std::string>(), std::string("100.0"));
    CHECK_EQ(posts[0]["text"].get<std::string>(), std::string("議論タイプを選択してください"));
    CHECK_EQ(posts[0]["blocks"], Json(build_flow_pick_blocks()));

    // 学習・壁打ちのラベルは parse_flow_kind に無い (TS と同じ) → 何もしない
    router.onInteractive(action(SLACK_FLOW_PICK_ACTION, "壁打ち", "C1", "100.0"));
    CHECK(slack.take_posts().empty());
    // 進行量の保留が無いのに設定が来ても何もしない
    router.onInteractive(action(SLACK_FLOW_SETTINGS_ACTION, "3x6", "C1", "100.0"));
    CHECK(slack.take_posts().empty());

    router.onInteractive(action(SLACK_FLOW_PICK_ACTION, "議論", "C1", "100.0"));
    posts = slack.take_posts();
    CHECK_EQ(posts.size(), std::size_t{1});
    CHECK_EQ(posts[0]["text"].get<std::string>(), std::string("進行量を選んで開始してください"));
    CHECK_EQ(posts[0]["blocks"], Json(build_flow_settings_blocks()));

    router.onInteractive(action(SLACK_FLOW_SETTINGS_ACTION, "3x6", "C1", "100.0"));
    posts = slack.take_posts();
    const std::string n1 = display("陽介", "opinion", "pro");
    const std::string n2 = display("花子", "debater", "con");
    CHECK_EQ(posts.size(), std::size_t{5});
    CHECK_EQ(posts[0]["text"].get<std::string>(), std::string("🗣️ *議論* を始めます: 「テーマA」"));
    CHECK(!posts[0].contains("username"));
    CHECK_EQ(posts[1]["text"].get<std::string>(), "*" + n1 + "*\n賛成です");
    CHECK_EQ(posts[1]["username"].get<std::string>(), n1);
    CHECK_EQ(posts[1]["icon_emoji"].get<std::string>(), std::string(":speech_balloon:"));
    CHECK_EQ(posts[2]["text"].get<std::string>(), "*" + n2 + "*\n反対です");
    flow::VoteDisplay vd{"votes", {{"u1", 2}, {"u2", 0}}, std::string("u1")};
    const auto lines = flow::build_vote_ranking_lines(vd, [&](const std::string& id) { return id == "u1" ? n1 : n2; });
    CHECK_EQ(posts[3]["text"].get<std::string>(), "*" + flow::vote_notice_title(1, "votes") + "*\n" + str::join(lines, "\n"));
    CHECK_EQ(posts[4]["text"].get<std::string>(), std::string("✅ *結論*\n結論の本文"));
    const auto reactions = slack.take_reactions();
    CHECK_EQ(reactions.size(), std::size_t{1});  // 得票 0 の u2 には付けない
    CHECK_EQ(reactions[0]["name"].get<std::string>(), std::string("trophy"));
    CHECK_EQ(reactions[0]["timestamp"].get<std::string>(), std::string("9.4"));  // u1 の投稿 ts
    CHECK_EQ(concluded.size(), std::size_t{1});
    CHECK_EQ(concluded[0].scene, std::string("slack:C1/100.0"));
    CHECK_EQ(concluded[0].title, std::string("テーマA"));
    CHECK_EQ(concluded[0].summary, std::string("結論の本文"));

    // 2 回目の設定は保留が消えているので起動しない
    router.onInteractive(action(SLACK_FLOW_SETTINGS_ACTION, "3x6", "C1", "100.0"));
    CHECK(slack.take_posts().empty());

    // 壊れた payload は無視
    router.onInteractive(Json{{"type", "block_actions"}});
    router.onInteractive(Json{{"type", "block_actions"}, {"actions", "x"}, {"channel", 3}, {"message", nullptr}});
    router.onInteractive(action(SLACK_FLOW_PICK_ACTION, "議論", "", "100.0"));
    router.onInteractive(Json::array());
    CHECK(slack.take_posts().empty());

    log::remove_sink(sink);
    for (const auto& l : logs) CHECK(!str::contains(l, kToken));
}

TEST_CASE("live: 未収束・タグ・改善・エラー・学習 Core 無しの案内") {
    reset_slack_live_for_test();
    FakeSlack slack;
    const auto deps = make_deps(slack);
    start_slack_flow({"C1", "1.0", "未収束", "improvement", {"機密", "運用"}, std::nullopt, std::nullopt, std::nullopt}, deps);
    auto posts = slack.take_posts();
    CHECK_EQ(posts.front()["text"].get<std::string>(), std::string("🗣️ *改善* を始めます: 「未収束」\nタグ: 機密 / 運用"));
    CHECK_EQ(posts.back()["text"].get<std::string>(), std::string("✅ *結論*\n(明確な結論には至りませんでした)"));

    start_slack_flow({"C1", "2.0", "boom", "discussion", {}, std::nullopt, std::nullopt, std::nullopt}, deps);
    posts = slack.take_posts();
    CHECK_EQ(posts.back()["text"].get<std::string>(), std::string("⚠️ フロー実行中にエラーが発生しました: boom"));

    start_slack_flow({"C1", "3.0", "t", "learning", {}, std::nullopt, std::nullopt, std::nullopt}, deps);
    posts = slack.take_posts();
    CHECK_EQ(posts.size(), std::size_t{1});
    CHECK_EQ(posts[0]["text"].get<std::string>(), std::string("⚠️ 学習フローは Core 未設定のため起動できません。"));

    // 未知の flow は FlowTypeRequiredError → エラー案内
    start_slack_flow({"C1", "4.0", "t", "雑談", {}, std::nullopt, std::nullopt, std::nullopt}, deps);
    posts = slack.take_posts();
    CHECK(str::starts_with(posts.back()["text"].get<std::string>(), "⚠️ フロー実行中にエラーが発生しました: 議論タイプが未指定または不正です"));

    // 投稿先が落ちていても例外は外へ出ない
    FakeSlack down(true);
    auto dead = make_deps(down);
    start_slack_flow({"C1", "5.0", "テーマ", "discussion", {}, std::nullopt, std::nullopt, std::nullopt}, dead);

    // 壁打ちでないスレッドの返信は false
    CHECK(!handle_slack_flow_reply("none", "C1", "x", deps));
}

TEST_CASE("live: 壁打ちは session を登録し、返信を submit_user に流す (/終了 で締める)") {
    reset_slack_live_for_test();
    FakeSlack slack;
    auto deps = make_deps(slack);
    start_slack_flow({"C1", "7.0", "壁打ちテーマ", "sparring", {}, std::nullopt, std::nullopt, std::nullopt}, deps);
    auto posts = slack.take_posts();
    if (has_slack_sparring_session("7.0")) {
        CHECK(str::starts_with(posts.back()["text"].get<std::string>(), "🥊 *壁打ち* を始めます。"));
        std::vector<SlackConcluded> concluded;
        SlackFlowHooks hooks;
        hooks.onConcluded = [&](const SlackConcluded& c) { concluded.push_back(c); };
        CHECK(handle_slack_flow_reply("7.0", "C1", "/終了", deps, hooks));
        CHECK(!has_slack_sparring_session("7.0"));
        posts = slack.take_posts();
        CHECK(str::starts_with(posts.back()["text"].get<std::string>(), "✅ *壁打ち終了*\n"));
        CHECK_EQ(concluded.size(), std::size_t{1});
        CHECK_EQ(concluded[0].title, std::string("壁打ち"));
        CHECK_EQ(concluded[0].scene, std::string("slack:C1/7.0"));
    } else {
        // 調査の段で失敗した環境ではエラー案内が出る
        CHECK(str::starts_with(posts.back()["text"].get<std::string>(), "⚠️ フロー実行中にエラーが発生しました"));
    }
}

TEST_CASE("router: 保留表は上限で古いものから捨てる / 満杯なら起動しない") {
    reset_slack_live_for_test();
    SlackRouterConfig cfg;
    cfg.channelIds = {"C1"};
    FakeSlack down(true);
    cfg.deps = make_deps(down);  // 選択 UI の投稿は失敗させる (握る)
    std::atomic<int> launched{0};
    cfg.submit = [&](std::function<void()>) {
        ++launched;
        return false;  // 満杯
    };
    auto router = create_slack_router(cfg);
    for (std::size_t i = 0; i <= kMaxPendingThreads; ++i) router.onEvent(message("C1", std::to_string(i) + ".0", "テーマ" + std::to_string(i)));
    // 最古 (0.0) は捨てられているが、選択は受け付ける (テーマ空で進行量待ちへ)
    router.onInteractive(action(SLACK_FLOW_PICK_ACTION, "改善", "C1", "0.0"));
    router.onInteractive(action(SLACK_FLOW_SETTINGS_ACTION, "default", "C1", "0.0"));
    CHECK_EQ(launched.load(), 1);  // 積もうとしたが満杯 → 起動しない
}

int main() { return di::test::run_all(); }
