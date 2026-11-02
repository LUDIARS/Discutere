// @spec Di管理フォーラムの受信境界 — tests/discord-hook/forum-event-gate.test.ts の移植 + 境界。
#include <map>
#include <mutex>

#include "check.hpp"

#include "discord_hook/discord_api.hpp"
#include "discord_hook/forum_event_gate.hpp"

using namespace di;
using namespace di::discord_hook;

namespace {

ForumChannelRef channel_fixture(const std::string& parent_id = "di-a", const std::string& guild_id = "guild-a") {
    ForumChannelRef parent;
    parent.id = parent_id;
    parent.guild_id = guild_id;
    parent.type = channel_type::GuildForum;
    ForumChannelRef c;
    c.id = "post";
    c.guild_id = guild_id;
    c.parent_id = parent_id;
    c.is_thread = true;
    c.type = channel_type::PublicThread;
    c.parent = std::make_shared<const ForumChannelRef>(parent);
    return c;
}

ForumMessageEvent message_fixture(const ForumChannelRef& channel = channel_fixture()) {
    ForumMessageEvent m;
    m.guild_id = channel.guild_id;
    m.channel_id = channel.id;
    m.channel = channel;
    return m;
}

struct Harness {
    ManagedForumIds forums;
    int callbacks = 0;
    int fetches = 0;
    ForumEventGate gate;
    Harness()
        : gate(bind_managed_forum_events([this] { return forums; },
                                         [this](const std::string&) -> std::optional<ForumChannelRef> {
                                             ++fetches;
                                             throw std::runtime_error("unexpected lookup");
                                         },
                                         ForumEventHandlers{[this](const ForumMessageEvent&) { ++callbacks; },
                                                            [this](const ForumReactionEvent&) { ++callbacks; },
                                                            [this](const ForumInteractionEvent&) { ++callbacks; },
                                                            [this](const ForumThreadEvent&, bool) { ++callbacks; }})) {}

    void emit(const ForumChannelRef& channel = channel_fixture()) {
        const auto message = message_fixture(channel);
        gate.on_message_create(message);
        gate.on_thread_create({channel, true, Json::object()});
        gate.on_reaction_add({message, false, Json::object()});
        for (const char* kind : {"button", "modal", "select", "slash"}) {
            ForumInteractionEvent i;
            i.guild_id = channel.guild_id;
            i.channel_id = channel.id;
            i.channel = channel;
            if (std::string(kind) != "slash") i.custom_id = "unrelated";
            gate.on_interaction_create(i);
        }
    }
};

} // namespace

TEST_CASE("forum event gate (forum-event-gate.test.ts)") {
    Harness h;
    // Ready has not resolved the managed IDs yet: no ingress callback or Discord lookup.
    h.emit();
    CHECK_EQ(h.callbacks + h.fetches, 0);
    h.forums = {{"guild-a", "di-a"}, {"guild-b", "di-b"}};
    auto not_thread = channel_fixture();
    not_thread.is_thread = false;
    auto text_parent = channel_fixture();
    ForumChannelRef p;
    p.id = "di-a";
    p.guild_id = "guild-a";
    p.type = channel_type::GuildText;
    text_parent.parent = std::make_shared<const ForumChannelRef>(p);
    for (const auto& c : {channel_fixture("other"), channel_fixture("di-a", "unconfigured"), channel_fixture("di-a", "guild-b"), not_thread, text_parent})
        h.emit(c);
    ForumMessageEvent dm;
    dm.channel_id = "dm";
    h.gate.on_message_create(dm);
    ForumInteractionEvent dm_slash;
    dm_slash.channel_id = "dm";
    h.gate.on_interaction_create(dm_slash);
    CHECK_EQ(h.callbacks + h.fetches, 0);

    auto reply = message_fixture();
    reply.has_reference = true;
    h.gate.on_message_create(reply);
    auto bot = message_fixture();
    bot.author_bot = true;
    h.gate.on_message_create(bot);
    h.gate.on_thread_create({channel_fixture(), false, Json::object()});
    h.gate.on_reaction_add({message_fixture(), true, Json::object()});
    CHECK_EQ(h.callbacks + h.fetches, 0);

    for (const char* prefix : {"flow-custom", "flow-modal", "flow-pick", "flow-settings", "paper-gap:mechanics"}) {
        const std::string custom_id = std::string(prefix) + ":another-post";
        CHECK(!component_matches_channel(custom_id, std::string("post")));
        ForumInteractionEvent i;
        i.guild_id = "guild-a";
        i.channel_id = "post";
        i.channel = channel_fixture();
        i.custom_id = custom_id;
        CHECK(!h.gate.on_interaction_create(i));
    }
    CHECK_EQ(h.callbacks + h.fetches, 0);
    CHECK(component_matches_channel("flow-pick:post", std::string("post")));
    CHECK(!component_matches_channel("flow-pick:post", std::nullopt));
    CHECK(component_matches_channel("unrelated", std::nullopt));
    h.emit();
    CHECK_EQ(h.callbacks, 7);
    h.emit(channel_fixture("di-b", "guild-b"));
    CHECK_EQ(h.callbacks, 14);
    CHECK_EQ(h.fetches, 0);
}

TEST_CASE("forum event gate: 未解決の親は fetch・失敗は拒否・ハンドラの例外は握りつぶす") {
    ManagedForumIds forums{{"guild-a", "di-a"}};
    int fetches = 0;
    bool fail = false;
    int handled = 0;
    ForumEventGate gate(
        [&] { return forums; },
        [&](const std::string& id) -> std::optional<ForumChannelRef> {
            ++fetches;
            if (fail) throw std::runtime_error("403");
            ForumChannelRef f;
            f.id = id;
            f.guild_id = "guild-a";
            f.type = channel_type::GuildForum;
            return f;
        },
        ForumEventHandlers{[&](const ForumMessageEvent&) {
                               ++handled;
                               throw std::runtime_error("handler secret");
                           },
                           nullptr, nullptr, nullptr});
    auto c = channel_fixture();
    c.parent = nullptr;  // 親が未キャッシュ
    auto m = message_fixture(c);
    CHECK(!gate.on_message_create(m));  // ハンドラは呼ばれたが例外
    CHECK_EQ(handled, 1);
    CHECK_EQ(fetches, 1);
    fail = true;
    CHECK(!gate.on_message_create(m));
    CHECK_EQ(handled, 1);
    // ハンドラの無いイベントは捨てる
    CHECK(!gate.on_thread_create({channel_fixture(), true, Json::object()}));
    CHECK(!gate.on_reaction_add({message_fixture(), false, Json::object()}));
    // forums の provider が投げても fail closed
    ForumEventGate broken([]() -> ManagedForumIds { throw std::runtime_error("x"); }, nullptr, ForumEventHandlers{[&](const ForumMessageEvent&) { ++handled; }, nullptr, nullptr, nullptr});
    CHECK(!broken.on_message_create(message_fixture()));
    CHECK_EQ(handled, 1);
}

DI_TEST_MAIN()
