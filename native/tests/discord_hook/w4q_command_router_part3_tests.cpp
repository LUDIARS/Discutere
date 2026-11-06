// w4q_command_router のテスト (3/4)。準備は w4q_command_router_support.hpp。
#include "w4q_command_router_support.hpp"

using namespace w4q_command_router_support;

TEST_CASE("handle_slash_interaction: 返信・admin 境界・壊れた入力・例外は internal error") {
    w4q::Env env;
    RecordingReplier rep;
    auto d = deps_with(env, {});
    CHECK(handle_slash_interaction(interaction("discutere-status", Json::array()), d, rep));
    CHECK_EQ(rep.replies.size(), std::size_t{1});
    CHECK(str::contains(rep.replies[0].content, "disabled"));
    // 壊れた interaction は返信しない
    CHECK(!handle_slash_interaction(Json{{"id", "x"}}, d, rep));
    CHECK(!handle_slash_interaction(Json("str"), d, rep));
    CHECK_EQ(rep.replies.size(), std::size_t{1});
    // route が例外 (KG を開けない) → internal error (例外の中身は返信に出さない)
    d.openCore = []() -> std::unique_ptr<core::Core> { throw std::runtime_error("/secret/path/kg.kuzu"); };
    CHECK(handle_slash_interaction(interaction("propose", Json::array()), d, rep));
    CHECK_EQ(rep.replies.back().content, std::string("⚠️ internal error"));
    CHECK(rep.replies.back().ephemeral);
    // 返信本文の形 (type 4 / ephemeral flag / メンションを解決しない)
    CHECK_EQ(json_stringify(slash_reply_body({"x", true})),
             std::string(R"({"type":4,"data":{"content":"x","flags":64,"allowed_mentions":{"parse":[]}}})"));
    CHECK_EQ(json_stringify(slash_reply_body({"y", false})), std::string(R"({"type":4,"data":{"content":"y","allowed_mentions":{"parse":[]}}})"));
}

TEST_CASE("Discatier コマンドは discord-session に記録 (同じ channel は同じ session)") {
    w4q::Env env;
    const auto d = deps_with(env, {});
    auto r = route_slash_command(slash("propose", std::string("u1"), "新しい仮説"), d);
    CHECK(!r.ephemeral);
    CHECK(str::starts_with(r.content, "✅ hypothesis "));
    route_slash_command(slash("integrate", std::string("u1")), d);
    CHECK_EQ(count(env, "SELECT COUNT(*) AS n FROM sessions WHERE title = 'discord-session:g1:ch1' AND scene = 'discord:g1/ch1'"),
             std::int64_t{1});
}

// ─── 平文 / フォーラムの取り込み ────────────────────────────────────────

TEST_CASE("route_inbound_message: 許可チャンネル (親継承)・空・slash は取り込まない・seed は分類器の結果") {
    w4q::Env env;
    auto d = deps_with(env, {});
    d.discussionChannelIds = {"chA"};
    std::vector<DiscordAutoDiscussionInput> seen;
    d.classifyInboundMessage = [&seen](const DiscordAutoDiscussionInput& in) {
        seen.push_back(in);
        return StarterResult{in.content == "議論しよう"};
    };
    CHECK(!route_inbound_message(message("chA", "   "), "g1", d).ingested);
    CHECK(!route_inbound_message(message("chA", "  /propose x"), "g1", d).ingested);
    auto r = route_inbound_message(message("chA", "  議論しよう  "), "g1", d);
    CHECK(r.ingested);
    CHECK(r.seed);
    CHECK(seen.empty());  // seed を呼ぶまで分類しない
    CHECK(r.seed());
    CHECK_EQ(seen[0].content, std::string("議論しよう"));
    CHECK_EQ(seen[0].authorId, std::string("u1"));
    // スレッドは親が許可なら継承 (session は実際の発言先)
    r = route_inbound_message(message("thread9", "雑談"), "g1", d, std::string("chA"));
    CHECK(r.ingested);
    CHECK(!r.seed());
    CHECK_EQ(count(env, "SELECT COUNT(*) AS n FROM sessions WHERE scene = 'discord:g1/thread9'"), std::int64_t{1});
    // 分類器の例外は false (落ちない)
    d.classifyInboundMessage = [](const DiscordAutoDiscussionInput&) -> StarterResult { throw std::runtime_error("boom"); };
    r = route_inbound_message(message("chA", "x"), "g1", d);
    CHECK(!r.seed());
    // 分類器未設定なら seed 無し
    d.classifyInboundMessage = nullptr;
    CHECK(!route_inbound_message(message("chA", "y"), "g1", d).seed);
}

TEST_CASE("handle_inbound_message_json: 正規化できない payload は取り込まない") {
    w4q::Env env;
    auto d = deps_with(env, {});
    d.discussionChannelIds = {"chA"};
    CHECK(!handle_inbound_message_json(Json::object(), "g1", d).ingested);
    CHECK(!handle_inbound_message_json(Json("x"), "g1", d).ingested);
    const Json ok{{"id", "m1"}, {"channel_id", "chA"}, {"content", "hello"}, {"timestamp", "t"}, {"author", Json{{"id", "u1"}, {"username", "a"}}}};
    CHECK(handle_inbound_message_json(ok, "g1", d).ingested);
    Json big = ok;
    big["content"] = std::string(1u << 20, 'x');
    CHECK(handle_inbound_message_json(big, "g1", d).ingested);
}

TEST_CASE("route_forum_post: starter だけ seed、open な議論があれば二重起動しない、許可リストに依らない") {
    w4q::Env env;
    auto d = deps_with(env, {});
    d.classifyInboundMessage = [](const DiscordAutoDiscussionInput& in) { return StarterResult{in.forumTitle.has_value()}; };
    ForumPostOptions starter;
    starter.isStarter = true;
    starter.forumTitle = "題";
    starter.direction = "improvement";
    starter.forumTags = std::vector<std::string>{"改善"};
    auto r = route_forum_post(message("T1", "本文"), "G1", d, starter);
    CHECK(r.ingested);
    CHECK(r.seed && r.seed());
    ForumPostOptions reply;
    CHECK(!route_forum_post(message("T1", "後続"), "G1", d, reply).seed);
    // open な議論 (scene=discord:G1/T1) が立っていれば starter でも seed しない
    env.kg_exec(Json::array({"INSERT INTO design_gaps (id, workspace_id, title, status, created_at, updated_at) VALUES ('g1', 'ws', 't', 'open', 1, 1)",
                             "INSERT INTO sessions (id, workspace_id, title, started_at, scene, created_at, updated_at) VALUES ('sg1', 'ws', "
                             "'discussion-of-gap:g1', 1, 'discord:G1/T1', 1, 1)"}));
    CHECK(!route_forum_post(message("T1", "再送"), "G1", d, starter).seed);
    // 終わった議論 (converged) なら立てられる
    env.kg_exec(Json::array({"UPDATE design_gaps SET status = 'converged' WHERE id = 'g1'"}));
    CHECK(route_forum_post(message("T1", "再開"), "G1", d, starter).seed);
    CHECK(!route_forum_post(message("T1", "/slash"), "G1", d, starter).ingested);
}

DI_TEST_MAIN()
