// @spec Di管理フォーラムの受信境界 — tests/discord-hook/managed-forum-scope.test.ts の移植 + 境界。
#include "check.hpp"
#include "discord_hook/managed_forum_scope.hpp"
#include "w1d_support.hpp"

using namespace di;
using namespace di::discord_hook;

namespace {
const ManagedForumIds forums{{"guild-a", "di-a"}, {"guild-b", "di-b"}};

ForumChannelRef forum(const std::string& id, const std::string& guild, int type = channel_type::GuildForum) {
    ForumChannelRef r;
    r.id = id;
    r.guild_id = guild;
    r.type = type;
    return r;
}

ForumChannelRef thread_fixture(const std::string& guild = "guild-a", const std::string& parent = "di-a") {
    ForumChannelRef t;
    t.id = "post";
    t.guild_id = guild;
    t.parent_id = parent;
    t.is_thread = true;
    t.type = channel_type::PublicThread;
    t.parent = std::make_shared<const ForumChannelRef>(forum(parent, guild));
    return t;
}
} // namespace

TEST_CASE("managed forum thread identity") {
    int fetches = 0;
    FetchForumChannel no_fetch = [&](const std::string&) -> std::optional<ForumChannelRef> {
        ++fetches;
        throw std::runtime_error("unexpected fetch");
    };
    auto a = thread_fixture();
    auto b = thread_fixture("guild-b", "di-b");
    CHECK(is_managed_forum_thread(&a, forums));
    CHECK(is_managed_forum_thread(&b, forums));
    CHECK(!is_managed_forum_thread(nullptr, forums));

    std::vector<ForumChannelRef> bad{thread_fixture("guild-a", "other"), thread_fixture("other-guild", "di-a"), thread_fixture("guild-b", "di-a")};
    auto not_thread = thread_fixture();
    not_thread.is_thread = false;
    bad.push_back(not_thread);
    auto text_parent = thread_fixture();
    text_parent.parent = std::make_shared<const ForumChannelRef>(forum("di-a", "guild-a", channel_type::GuildText));
    bad.push_back(text_parent);
    auto empty_id = thread_fixture();
    empty_id.id = "";
    bad.push_back(empty_id);
    for (const auto& c : bad) {
        CHECK(!is_managed_forum_thread(&c, forums));
        CHECK(!is_managed_forum_location({c.guild_id, c.id, c}, forums, no_fetch));
    }
    CHECK_EQ(fetches, 0);
    CHECK(!is_managed_forum_location({std::nullopt, std::string("dm"), std::nullopt}, forums, no_fetch));
    CHECK(!is_managed_forum_location({std::string("guild-a"), std::string("post"), std::nullopt}, {}, no_fetch));  // 空の scope は全拒否
    CHECK(!is_managed_forum_location({std::string("guild-a"), std::string(""), std::nullopt}, forums, no_fetch));
    CHECK(!is_managed_forum_location({std::string(""), std::string("post"), std::nullopt}, forums, no_fetch));
    CHECK(!is_managed_forum_location({std::string("guild-a"), std::string("post"), std::nullopt}, ManagedForumIds{{"guild-a", ""}}, no_fetch));
    CHECK_EQ(fetches, 0);
}

TEST_CASE("uncached parent is resolved by read-only fetch and fails closed") {
    auto uncached = thread_fixture();
    uncached.parent = nullptr;
    int fetches = 0;
    CHECK(is_managed_forum_location({std::string("guild-a"), std::string("post"), uncached}, forums,
                                    [&](const std::string& id) -> std::optional<ForumChannelRef> {
                                        CHECK_EQ(id, std::string("di-a"));
                                        ++fetches;
                                        return forum("di-a", "guild-a");
                                    }));
    CHECK_EQ(fetches, 1);
    std::vector<std::optional<ForumChannelRef>> parents{std::nullopt, forum("other", "guild-a"), forum("di-a", "other-guild"),
                                                        forum("di-a", "guild-a", channel_type::GuildText)};
    for (const auto& p : parents)
        CHECK(!is_managed_forum_location({std::string("guild-a"), std::string("post"), uncached}, forums,
                                         [&](const std::string&) { return p; }));
    CHECK(!is_managed_forum_location({std::string("guild-a"), std::string("post"), uncached}, forums,
                                     [](const std::string&) -> std::optional<ForumChannelRef> { throw std::runtime_error("cannot fetch parent"); }));
    // チャンネル自体も未取得なら取得して照合する
    CHECK(is_managed_forum_location({std::string("guild-a"), std::string("post"), std::nullopt}, forums,
                                    [](const std::string& id) -> std::optional<ForumChannelRef> {
                                        if (id == "post") return thread_fixture();
                                        return std::nullopt;
                                    }));
    // 渡されたチャンネルと channelId の不一致は拒否
    CHECK(!is_managed_forum_location({std::string("guild-a"), std::string("wrong"), thread_fixture()}, forums,
                                     [](const std::string&) -> std::optional<ForumChannelRef> { throw std::runtime_error("x"); }));
    // fetch 関数が無ければ未解決は拒否
    CHECK(!is_managed_forum_location({std::string("guild-a"), std::string("post"), uncached}, forums, {}));
}

TEST_CASE("REST fetcher against fake Discord: thread in managed forum only") {
    w1d::FakeServer fake;
    fake.router->get("/channels/:id", [](http::Request& r) {
        const std::string id = r.param("id");
        if (id == "post") return http::Response::json(Json{{"id", "post"}, {"type", 11}, {"guild_id", "guild-a"}, {"parent_id", "di-a"}});
        if (id == "di-a") return http::Response::json(Json{{"id", "di-a"}, {"type", 15}, {"guild_id", "guild-a"}});
        if (id == "text") return http::Response::json(Json{{"id", "text"}, {"type", 0}, {"guild_id", "guild-a"}, {"parent_id", "di-a"}});
        return http::Response::json(Json{{"message", "Missing Access"}, {"code", 50001}}, 403);
    });
    fake.start();
    DiscordApiOptions o;
    o.token = "fake-token";
    o.base_url = fake.base;
    o.allow_private_network = true;
    DiscordApi api(o);
    auto fetch = rest_forum_channel_fetcher(api);
    CHECK(is_managed_forum_location({std::string("guild-a"), std::string("post"), std::nullopt}, forums, fetch));
    CHECK(!is_managed_forum_location({std::string("guild-a"), std::string("text"), std::nullopt}, forums, fetch));
    CHECK(!is_managed_forum_location({std::string("guild-a"), std::string("denied"), std::nullopt}, forums, fetch));
    CHECK(!is_managed_forum_location({std::string("guild-b"), std::string("post"), std::nullopt}, forums, fetch));
}

DI_TEST_MAIN()
