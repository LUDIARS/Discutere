// REST を使う discord-hook (forum-flow-tags の ensure・system-channel・managed-channels・game-feedback・poster) を
// 偽の Discord REST サーバで確かめる。discord.js のキャッシュの代わりに REST で探すこと、権限の判定、失敗時の縮退。
#include <atomic>
#include <mutex>

#include "base/strings.hpp"
#include "check.hpp"
#include "discord_hook/forum_flow_tags.hpp"
#include "discord_hook/game_feedback_channel.hpp"
#include "discord_hook/managed_channels.hpp"
#include "discord_hook/paper_review_ack.hpp"
#include "discord_hook/poster.hpp"
#include "discord_hook/system_channel.hpp"
#include "w1d_support.hpp"

using namespace di;
using namespace di::discord_hook;

namespace {

/** 1 guild ぶんの状態を持つ偽 Discord。 */
struct FakeDiscord {
    w1d::FakeServer fake;
    std::mutex m;
    Json channels = Json::array();
    std::uint64_t everyone_perms = 0;
    std::vector<Json> posts;     // POST /guilds/g/channels の本文
    std::vector<Json> patches;   // PATCH /channels/:id の本文
    std::vector<std::string> webhook_queries;
    std::vector<Json> messages;
    std::atomic<int> webhook_lists{0}, webhook_creates{0};
    int next = 100;

    FakeDiscord() {
        auto& r = *fake.router;
        r.get("/users/@me", [](http::Request&) { return http::Response::json(Json{{"id", "bot"}, {"username", "Di"}, {"bot", true}}); });
        r.get("/guilds/:id", [this](http::Request& q) {
            if (q.param("id") != "g") return http::Response::json(Json{{"message", "Unknown Guild"}, {"code", 10004}}, 404);
            std::lock_guard lock(m);
            return http::Response::json(Json{{"id", "g"}, {"name", "G"}, {"owner_id", "someone"},
                                             {"roles", Json::array({Json{{"id", "g"}, {"name", "@everyone"}, {"permissions", std::to_string(everyone_perms)}}})}});
        });
        r.get("/guilds/:id/members/:u", [](http::Request&) { return http::Response::json(Json{{"user", {{"id", "bot"}, {"username", "Di"}}}, {"roles", Json::array()}}); });
        r.get("/guilds/:id/channels", [this](http::Request&) {
            std::lock_guard lock(m);
            return http::Response::json(channels);
        });
        r.post("/guilds/:id/channels", [this](http::Request& q) {
            std::lock_guard lock(m);
            Json body = Json::parse(q.body);
            posts.push_back(body);
            Json ch = body;
            ch["id"] = std::to_string(next++);
            ch["guild_id"] = "g";
            channels.push_back(ch);
            return http::Response::json(ch, 201);
        });
        r.patch("/channels/:id", [this](http::Request& q) {
            std::lock_guard lock(m);
            Json body = Json::parse(q.body);
            patches.push_back(body);
            Json ch{{"id", q.param("id")}, {"type", 15}, {"guild_id", "g"}};
            for (auto& [k, v] : body.items()) ch[k] = v;
            return http::Response::json(ch);
        });
        r.get("/channels/:id", [](http::Request& q) {
            if (q.param("id") == "thread") return http::Response::json(Json{{"id", "thread"}, {"type", 11}, {"parent_id", "forum"}});
            if (q.param("id") == "missing") return http::Response::json(Json{{"message", "Unknown Channel"}, {"code", 10003}}, 404);
            return http::Response::json(Json{{"id", q.param("id")}, {"type", 0}});
        });
        r.get("/channels/:id/webhooks", [this](http::Request&) {
            ++webhook_lists;
            return http::Response::json(Json::array({Json{{"id", "w0"}, {"name", "other"}, {"token", "t0"}}}));
        });
        r.post("/channels/:id/webhooks", [this](http::Request& q) {
            ++webhook_creates;
            CHECK_EQ(Json::parse(q.body)["name"].get<std::string>(), std::string("Discutere議論"));
            return http::Response::json(Json{{"id", "w1"}, {"name", "Discutere議論"}, {"token", "fake-webhook-token"}});
        });
        r.post("/webhooks/:id/:token", [this](http::Request& q) {
            std::lock_guard lock(m);
            CHECK(!q.header("Authorization"));  // webhook 実行に bot token を送らない
            webhook_queries.push_back(q.query_string);
            messages.push_back(Json::parse(q.body));
            if (q.param("id") == "broken") return http::Response::text("not json");
            return http::Response::json(Json{{"id", "wm1"}, {"channel_id", "x"}});
        });
        r.post("/channels/:id/messages", [this](http::Request& q) {
            std::lock_guard lock(m);
            messages.push_back(Json::parse(q.body));
            return http::Response::json(Json{{"id", "m9"}, {"channel_id", q.param("id")}});
        });
        r.put("/channels/:c/messages/:m/reactions/:e/@me", [](http::Request& q) {
            if (q.param("c") == "noperm") return http::Response::json(Json{{"message", "Missing Permissions"}, {"code", 50013}}, 403);
            return http::Response::empty(204);
        });
        fake.start();
    }

    DiscordApiOptions options() const {
        DiscordApiOptions o;
        o.token = "fake-token";
        o.base_url = fake.base;
        o.allow_private_network = true;
        return o;
    }
};

} // namespace

TEST_CASE("ensure_discussion_forum creates once, then supplements tags") {
    FakeDiscord d;
    DiscordApi api(d.options());
    auto forum = ensure_discussion_forum(api, "g", "議論");
    CHECK(forum.has_value());
    CHECK_EQ(d.posts.size(), std::size_t(1));
    CHECK_EQ(d.posts[0]["type"].get<int>(), channel_type::GuildForum);
    CHECK_EQ(d.posts[0]["available_tags"].size(), std::size_t(8));
    // 既存 (タグ不足 + 廃止タグ) は流用して PATCH で補充、作成はしない
    {
        std::lock_guard lock(d.m);
        d.channels = Json::array({Json{{"id", "f1"}, {"type", 15}, {"name", "議論"}, {"guild_id", "g"},
                                       {"available_tags", Json::array({Json{{"id", "t1"}, {"name", "議論"}}, Json{{"id", "t2"}, {"name", "壁打ち"}}})}}});
    }
    auto again = ensure_discussion_forum(api, "g", "議論");
    CHECK_EQ(again->id, std::string("f1"));
    CHECK_EQ(d.posts.size(), std::size_t(1));
    CHECK_EQ(d.patches.size(), std::size_t(1));
    const Json& tags = d.patches[0]["available_tags"];
    CHECK_EQ(tags.size(), std::size_t(8));
    CHECK_EQ(tags[0]["id"].get<std::string>(), std::string("t1"));
    // 全タグがそろっていれば PATCH しない
    Channel full;
    full.id = "f2";
    for (const auto& n : all_flow_tag_names()) full.available_tags.push_back(ForumTag{std::nullopt, n, std::nullopt, std::nullopt, std::nullopt});
    CHECK(ensure_forum_flow_tags(api, full).empty());
    CHECK_EQ(d.patches.size(), std::size_t(1));
    // 未知の guild は nullopt
    CHECK(!ensure_discussion_forum(api, "unknown", "議論"));
}

TEST_CASE("system channel respects Manage Channels and caches") {
    FakeDiscord d;
    DiscordApi api(d.options());
    clear_system_channel_cache();
    // 権限なし + 既存なし → nullopt (作成しない)
    CHECK(!ensure_system_channel(api, {"g", std::nullopt, std::nullopt, std::nullopt}));
    CHECK(d.posts.empty());
    // 権限あり → カテゴリとチャンネルを作る
    d.everyone_perms = permission::ManageChannels;
    auto ch = ensure_system_channel(api, {"g", std::nullopt, std::nullopt, std::nullopt});
    CHECK(ch.has_value());
    CHECK_EQ(d.posts.size(), std::size_t(2));
    CHECK_EQ(d.posts[0]["type"].get<int>(), channel_type::GuildCategory);
    CHECK_EQ(d.posts[1]["parent_id"].get<std::string>(), std::string("100"));  // 作ったカテゴリの下
    CHECK_EQ(d.posts[1]["topic"].get<std::string>(), std::string("外部議論データの取り込み通知 (Discutere クロール)"));
    // 2 回目はキャッシュ
    auto again = ensure_system_channel(api, {"g", std::nullopt, std::nullopt, std::nullopt});
    CHECK_EQ(again->id, ch->id);
    CHECK_EQ(d.posts.size(), std::size_t(2));
    // 権限なしでも既存の同名 (正規化して照合) は流用する
    clear_system_channel_cache();
    d.everyone_perms = 0;
    {
        std::lock_guard lock(d.m);
        d.channels = Json::array({Json{{"id", "ex"}, {"type", 0}, {"name", "my-channel"}, {"guild_id", "g"}}});
    }
    CHECK_EQ(ensure_system_channel(api, {"g", std::nullopt, std::string("My Channel"), std::nullopt})->id, std::string("ex"));
    CHECK_EQ(normalize_channel_name("A  B\tC"), std::string("a-b-c"));
    CHECK(!ensure_system_channel(api, {"unknown", std::nullopt, std::nullopt, std::nullopt}));
}

TEST_CASE("managed channels and game feedback category") {
    FakeDiscord d;
    DiscordApi api(d.options());
    clear_system_channel_cache();
    d.everyone_perms = permission::Administrator;  // Administrator は全権限
    auto res = ensure_managed_channels(api, {"g", "unknown"});
    CHECK_EQ(res.dataLearningChannelIds.size(), std::size_t(1));
    std::size_t texts = 0, cats = 0;
    for (const auto& c : d.channels) {
        if (c["type"] == channel_type::GuildText) ++texts;
        if (c["type"] == channel_type::GuildCategory) ++cats;
    }
    CHECK_EQ(texts, std::size_t(2));
    CHECK_EQ(cats, std::size_t(1));  // カテゴリ「システム」は 1 つだけ
    const std::size_t before = d.posts.size();
    ensure_game_feedback_category(api, {"g", "unknown"}, "ゲーム感想");
    CHECK_EQ(d.posts.size(), before + 1);
    ensure_game_feedback_category(api, {"g"}, "ゲーム感想");  // 既存なら作らない
    CHECK_EQ(d.posts.size(), before + 1);
}

TEST_CASE("poster: channel post, webhook ensure/cache, thread target, webhook post") {
    FakeDiscord d;
    DiscordApi api(d.options());
    clear_webhook_cache();
    const std::string long_text(3000, 'a');
    CHECK_EQ(post_discord_channel(api, {"c1", long_text, std::string("p1")}), std::string("m9"));
    CHECK_EQ(str::js_length(d.messages.back()["content"].get<std::string>()), std::size_t(1900));
    CHECK_EQ(d.messages.back()["message_reference"]["message_id"].get<std::string>(), std::string("p1"));
    CHECK_EQ(d.messages.back()["allowed_mentions"].dump(), std::string(R"({"replied_user":false})"));

    auto hook = ensure_channel_webhook(api, "forum");
    CHECK_EQ(hook.id, std::string("w1"));
    CHECK_EQ(d.webhook_creates.load(), 1);
    ensure_channel_webhook(api, "forum");
    CHECK_EQ(d.webhook_lists.load(), 1);  // 2 回目は保持から

    auto target = resolve_webhook_target(api, "thread");
    CHECK_EQ(target.webhookChannelId, std::string("forum"));
    CHECK_EQ(*target.threadId, std::string("thread"));
    auto plain = resolve_webhook_target(api, "missing");
    CHECK_EQ(plain.webhookChannelId, std::string("missing"));
    CHECK(!plain.threadId);

    CHECK_EQ(post_discord_webhook(api, {hook.id, hook.token, std::string(100, 'n'), "x", std::nullopt, std::string("thread")}), std::string("wm1"));
    CHECK(str::contains(d.webhook_queries.back(), "wait=true"));
    CHECK(str::contains(d.webhook_queries.back(), "thread_id=thread"));
    CHECK_EQ(d.messages.back()["username"].get<std::string>().size(), std::size_t(80));
    CHECK(!d.messages.back().contains("avatar_url"));
    CHECK_EQ(post_discord_webhook(api, {"broken", "tok", "u", "x", std::nullopt, std::nullopt}), std::string(""));

    react_discord(api, "c1", "m1", "👍");
    CHECK_THROWS(react_discord(api, "noperm", "m1", "👍"));
    // paper-review-ack: 権限エラーは warn で済ませる
    Message msg;
    msg.id = "m1";
    msg.channel_id = "noperm";
    msg.content = "はい";
    std::vector<std::string> warns;
    CHECK(ack_paper_review_reply(ackable_message(api, msg), true, [&](const std::string& w) { warns.push_back(w); }));
    CHECK_EQ(warns.size(), std::size_t(1));
    CHECK(!str::contains(warns[0], "fake-token"));
}

DI_TEST_MAIN()
