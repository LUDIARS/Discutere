// w4-Q: discord-hook/forum-monitor.ts の移植の検証。
// tests/discord-hook/forum-monitor.test.ts の移植 (golden: Node の pickForumDirection / parseDiscordScene / formatForumSummary と一致) と、
// 偽の Discord (loopback の http::Server) に対する ThreadCreate の起動・後続投稿・収束クローズ。
#include "check.hpp"
#include "w1d_support.hpp"
#include "w4q_support.hpp"

#include <mutex>

#include "base/strings.hpp"
#include "discord_hook/forum_monitor.hpp"
#include "discord_hook/system_channel.hpp"

using namespace di;
using namespace di::discord_hook;

TEST_CASE("golden: pickForumDirection / parseDiscordScene / formatForumSummary") {
    const auto fx = w4q::load_fixture("forum.json");
    for (const auto& d : fx["directions"])
        CHECK_EQ(pick_forum_direction(d["tags"].get<std::vector<std::string>>()), d["direction"].get<std::string>());
    CHECK_EQ(pick_forum_direction({"proposal"}, ForumDirectionConfig{{"proposal"}, {"fun"}}), fx["custom"].get<std::string>());
    for (const auto& s : fx["scenes"]) {
        const auto parsed = parse_discord_scene(s["scene"].get<std::string>());
        if (s["parsed"].is_null()) {
            CHECK(!parsed);
        } else {
            CHECK(parsed.has_value());
            CHECK_EQ(parsed->guildId, s["parsed"]["guildId"].get<std::string>());
            CHECK_EQ(parsed->channelId, s["parsed"]["channelId"].get<std::string>());
        }
    }
    CHECK(!parse_discord_scene(std::nullopt));
    for (const auto& s : fx["summaries"])
        CHECK_EQ(format_forum_summary(s["title"].get<std::string>(), s["summary"].get<std::string>()), s["out"].get<std::string>());
}

TEST_CASE("isForumThreadChannel / isForumStarterMessage") {
    auto forum_parent = std::make_shared<ForumChannelRef>();
    forum_parent->type = channel_type::GuildForum;
    auto text_parent = std::make_shared<ForumChannelRef>();
    text_parent->type = channel_type::GuildText;
    ForumChannelRef thread;
    thread.is_thread = true;
    thread.parent = forum_parent;
    CHECK(is_forum_thread_channel(&thread));
    thread.parent = text_parent;
    CHECK(!is_forum_thread_channel(&thread));  // 親がテキスト = フォーラムでない
    thread.parent = nullptr;
    CHECK(!is_forum_thread_channel(&thread));
    ForumChannelRef plain;
    plain.parent = forum_parent;
    CHECK(!is_forum_thread_channel(&plain));
    CHECK(!is_forum_thread_channel(nullptr));
    CHECK(is_forum_starter_message("999", "999"));
    CHECK(!is_forum_starter_message("1000", "999"));
}

TEST_CASE("thread_applied_tag_names: 親の availableTags で名前に解決 (未知の id・空名は落とす)") {
    Channel thread;
    thread.applied_tags = {"t1", "t9", "t2"};
    Channel forum;
    forum.available_tags = {ForumTag{std::string("t1"), "改善", {}, {}, {}}, ForumTag{std::string("t2"), "", {}, {}, {}}};
    CHECK_EQ(thread_applied_tag_names(thread, forum), (std::vector<std::string>{"改善"}));
    CHECK(thread_applied_tag_names(thread, std::nullopt).empty());
}

namespace {

struct FakeDiscord {
    w1d::FakeServer fake;
    std::mutex m;
    std::vector<std::string> reactions;
    std::vector<Json> posted;
    std::vector<Json> patches;
    int starter_gets = 0;
    int starter_fail_first = 0;
    bool starter_bot = false;
    int archive_status = 200;
    FakeDiscord() {
        auto& r = *fake.router;
        r.get("/users/@me", [](http::Request&) { return http::Response::json(Json{{"id", "bot"}, {"username", "b"}, {"bot", true}}); });
        r.get("/guilds/:id", [](http::Request& q) { return http::Response::json(Json{{"id", q.param("id")}, {"name", "g"}, {"owner_id", "bot"}, {"roles", Json::array()}}); });
        r.get("/guilds/:id/channels", [](http::Request&) {
            return http::Response::json(Json::array({Json{{"id", "cat"}, {"type", 4}, {"name", "システム"}},
                                                     Json{{"id", "sum"}, {"type", 0}, {"name", "まとめ投稿"}, {"parent_id", "cat"}}}));
        });
        r.get("/channels/:id", [](http::Request& q) {
            const auto id = q.param("id");
            if (id == "F1")
                return http::Response::json(Json{{"id", "F1"}, {"type", 15}, {"guild_id", "G1"},
                                                 {"available_tags", Json::array({Json{{"id", "t1"}, {"name", "改善提案"}}})}});
            if (id == "TX") return http::Response::json(Json{{"id", "TX"}, {"type", 0}, {"guild_id", "G1"}});
            if (id == "T2") return http::Response::json(Json{{"id", "T2"}, {"type", 11}, {"guild_id", "G1"}, {"parent_id", "TX"}});
            if (id == "T1") return http::Response::json(Json{{"id", "T1"}, {"type", 11}, {"guild_id", "G1"}, {"parent_id", "F1"}});
            return http::Response::json(Json{{"message", "Unknown Channel"}, {"code", 10003}}, 404);
        });
        r.get("/channels/:id/messages/:mid", [this](http::Request& q) {
            std::lock_guard lock(m);
            ++starter_gets;
            if (starter_fail_first-- > 0) return http::Response::json(Json{{"message", "Unknown Message"}, {"code", 10008}}, 404);
            return http::Response::json(Json{{"id", q.param("mid")},
                                             {"channel_id", q.param("id")},
                                             {"content", "  ローグライトの周回はどうか  "},
                                             {"timestamp", "2026-01-01T00:00:00.000Z"},
                                             {"author", Json{{"id", "u1"}, {"username", "alice"}, {"bot", starter_bot}}},
                                             {"mentions", Json::array()}});
        });
        r.put("/channels/:c/messages/:m/reactions/:e/@me", [this](http::Request& q) {
            std::lock_guard lock(m);
            reactions.push_back(q.param("c") + "/" + q.param("m") + "/" + q.param("e"));
            return http::Response::empty(204);
        });
        r.post("/channels/:id/messages", [this](http::Request& q) {
            std::lock_guard lock(m);
            Json b = Json::parse(q.body);
            b["_channel"] = q.param("id");
            posted.push_back(b);
            return http::Response::json(Json{{"id", "m9"}, {"channel_id", q.param("id")}, {"content", ""}, {"timestamp", "t"},
                                             {"author", Json{{"id", "bot"}, {"username", "b"}}}});
        });
        r.patch("/channels/:id", [this](http::Request& q) {
            std::lock_guard lock(m);
            Json b = Json::parse(q.body);
            patches.push_back(b);
            if (b.contains("archived") && archive_status != 200)
                return http::Response::json(Json{{"message", "Missing Permissions"}, {"code", 50013}}, archive_status);
            Json ch{{"id", q.param("id")}, {"type", 11}, {"guild_id", "G1"}, {"parent_id", "F1"}};
            return http::Response::json(ch);
        });
        fake.start();
        clear_system_channel_cache();
    }
    ~FakeDiscord() { clear_system_channel_cache(); }
    DiscordApiOptions options() const {
        DiscordApiOptions o;
        o.token = "fake-token";
        o.base_url = fake.base;
        o.allow_private_network = true;
        o.max_retries = 0;
        return o;
    }
};

Json thread_json(const std::string& id = "T1", const std::string& parent = "F1") {
    return Json{{"id", id}, {"type", 11}, {"guild_id", "G1"}, {"parent_id", parent}, {"name", "周回の議論"}, {"applied_tags", Json::array({"t1"})}};
}

ForumMonitorDeps monitor_deps(const w4q::Env& env, std::vector<DiscordAutoDiscussionInput>& seen, bool started = true) {
    ForumMonitorDeps d;
    d.router.workspaceId = "ws";
    const auto kg = env.kg;
    const auto events = env.dir.file("events.jsonl");
    d.router.openCore = [kg, events] { return core::create_core(kg, events); };
    d.router.classifyInboundMessage = [&seen, started](const DiscordAutoDiscussionInput& in) {
        seen.push_back(in);
        return StarterResult{started};
    };
    d.sleep = [](std::chrono::milliseconds) {};
    return d;
}

} // namespace

TEST_CASE("ThreadCreate: starter を取り込み、議論が立てば 👀 (方向性・タグ・スレッド名を渡す)") {
    w4q::Env env;
    FakeDiscord d;
    DiscordApi api(d.options());
    std::vector<DiscordAutoDiscussionInput> seen;
    handle_forum_thread_create(thread_json(), api, monitor_deps(env, seen));
    CHECK_EQ(seen.size(), std::size_t{1});
    CHECK_EQ(seen[0].channelId, std::string("T1"));
    CHECK_EQ(seen[0].guildId, std::string("G1"));
    CHECK_EQ(seen[0].content, std::string("ローグライトの周回はどうか"));
    CHECK_EQ(seen[0].forumTitle.value_or(""), std::string("周回の議論"));
    CHECK_EQ(seen[0].direction.value_or(""), std::string("improvement"));
    CHECK(seen[0].forumTags && *seen[0].forumTags == std::vector<std::string>{"改善提案"});
    CHECK_EQ(d.reactions.size(), std::size_t{1});
    CHECK(str::starts_with(d.reactions[0], "T1/T1/"));
    auto c = env.core();
    CHECK_EQ(c->raw().get("SELECT COUNT(*) AS n FROM sessions WHERE scene = 'discord:G1/T1'")->i64("n"), std::int64_t{1});
    c->close();
}

TEST_CASE("ThreadCreate: 議論が立たなければ 👀 なし・starter 取得は 1 回リトライ・bot の starter は無視") {
    w4q::Env env;
    FakeDiscord d;
    DiscordApi api(d.options());
    std::vector<DiscordAutoDiscussionInput> seen;
    d.starter_fail_first = 1;
    handle_forum_thread_create(thread_json(), api, monitor_deps(env, seen, false));
    CHECK_EQ(d.starter_gets, 2);
    CHECK_EQ(seen.size(), std::size_t{1});
    CHECK(d.reactions.empty());
    // 2 回とも取れなければ諦める
    d.starter_fail_first = 2;
    handle_forum_thread_create(thread_json(), api, monitor_deps(env, seen, false));
    CHECK_EQ(seen.size(), std::size_t{1});
    d.starter_bot = true;
    d.starter_fail_first = 0;
    handle_forum_thread_create(thread_json(), api, monitor_deps(env, seen, true));
    CHECK_EQ(seen.size(), std::size_t{1});
}

TEST_CASE("ThreadCreate: 親が forum でない・親が取れない・壊れた JSON は何もしない") {
    w4q::Env env;
    FakeDiscord d;
    DiscordApi api(d.options());
    std::vector<DiscordAutoDiscussionInput> seen;
    handle_forum_thread_create(thread_json("T2", "TX"), api, monitor_deps(env, seen));
    handle_forum_thread_create(thread_json("T3", "missing"), api, monitor_deps(env, seen));
    handle_forum_thread_create(Json("broken"), api, monitor_deps(env, seen));
    handle_forum_thread_create(Json{{"id", "T5"}, {"type", 11}}, api, monitor_deps(env, seen));
    CHECK(seen.empty());
    CHECK_EQ(d.starter_gets, 0);
}

TEST_CASE("後続投稿: utterance のみ (分類しない)・guild 無しは無視") {
    w4q::Env env;
    std::vector<DiscordAutoDiscussionInput> seen;
    const auto deps = monitor_deps(env, seen);
    const Json msg{{"id", "m2"}, {"channel_id", "T1"}, {"guild_id", "G1"}, {"content", "後続の意見"}, {"timestamp", "t"},
                   {"author", Json{{"id", "u2"}, {"username", "bob"}}}};
    handle_forum_reply(msg, deps);
    Json no_guild = msg;
    no_guild.erase("guild_id");
    handle_forum_reply(no_guild, deps);
    handle_forum_reply(Json::array(), deps);
    CHECK(seen.empty());
    auto c = env.core();
    CHECK_EQ(c->raw().get("SELECT COUNT(*) AS n FROM utterances WHERE raw_content = '後続の意見'")->i64("n"), std::int64_t{1});
    c->close();
}

TEST_CASE("finalize_forum_post: まとめ転記 (メンション無効) → lock → archive") {
    FakeDiscord d;
    DiscordApi api(d.options());
    FinalizeForumPostArgs a;
    a.scene = "discord:G1/T1";
    a.title = "周回の議論";
    a.summary = "  ビルド多様性 @everyone  ";
    const auto r = finalize_forum_post(api, a);
    CHECK(r.closed);
    CHECK_EQ(d.posted.size(), std::size_t{1});
    CHECK_EQ(d.posted[0]["_channel"].get<std::string>(), std::string("sum"));
    CHECK_EQ(d.posted[0]["content"].get<std::string>(),
             std::string("✅ **収束**: 周回の議論\nビルド多様性 @everyone\nhttps://discord.com/channels/G1/T1"));
    CHECK_EQ(json_stringify(d.posted[0]["allowed_mentions"]), std::string(R"({"parse":[]})"));
    CHECK_EQ(d.patches.size(), std::size_t{2});
    CHECK(d.patches[0]["locked"].get<bool>());
    CHECK(d.patches[1]["archived"].get<bool>());
}

TEST_CASE("finalize_forum_post: discord 以外の scene・フォーラムでない・取得失敗・archive 失敗") {
    FakeDiscord d;
    DiscordApi api(d.options());
    FinalizeForumPostArgs a;
    a.scene = "web:lobby";
    CHECK_EQ(finalize_forum_post(api, a).reason.value_or(""), std::string("scene is not discord-bound"));
    a.scene = std::nullopt;
    CHECK(!finalize_forum_post(api, a).closed);
    a.scene = "discord:G1/T2";
    CHECK_EQ(finalize_forum_post(api, a).reason.value_or(""), std::string("not a forum thread"));
    a.scene = "discord:G1/TX";
    CHECK_EQ(finalize_forum_post(api, a).reason.value_or(""), std::string("not a forum thread"));
    a.scene = "discord:G1/gone";
    const auto r = finalize_forum_post(api, a);
    CHECK(str::starts_with(r.reason.value_or(""), "channel fetch failed: "));
    CHECK(!str::contains(r.reason.value_or(""), "fake-token"));
    CHECK(d.patches.empty());
    d.archive_status = 403;
    a.scene = "discord:G1/T1";
    const auto r2 = finalize_forum_post(api, a);
    CHECK(!r2.closed);
    CHECK(str::starts_with(r2.reason.value_or(""), "archive failed: "));
}

DI_TEST_MAIN()
