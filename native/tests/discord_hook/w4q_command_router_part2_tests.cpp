// w4q_command_router のテスト (2/4)。準備は w4q_command_router_support.hpp。
#include "w4q_command_router_support.hpp"

using namespace w4q_command_router_support;

TEST_CASE("8. route_inbound_message は非許可チャンネルを早期 return (core に触れない)") {
    w4q::Env env;
    auto d = deps_with(env, {"admin1"});
    std::atomic<int> opened{0};
    d.openCore = [&opened]() -> std::unique_ptr<core::Core> {
        ++opened;
        throw std::runtime_error("must not open");
    };
    const auto r1 = route_inbound_message(message("ch-not-allowed", "ふつうの発言"), "g1", d);
    CHECK(!r1.ingested);
    CHECK(!r1.seed);
    d.discussionChannelIds = {"ch-other"};
    const auto r2 = route_inbound_message(message("ch-not-allowed", "ふつうの発言"), "g1", d);
    CHECK(!r2.ingested);
    // 親が空文字でも許可しない
    CHECK(!route_inbound_message(message("ch-not-allowed", "x"), "g1", d, std::string("")).ingested);
    CHECK_EQ(opened.load(), 0);
}

TEST_CASE("9. /discutere-discussions — 議論一覧 (state 絞り込み) を ephemeral で返す") {
    const auto fx = w4q::load_fixture("slash.json");
    w4q::Env env;
    env.flow_exec(fx["flowSetup"]);
    const auto d = deps_with(env, {});
    const auto all = route_slash_command(slash("discutere-discussions", std::nullopt, ""), d);
    CHECK(all.ephemeral);
    CHECK(str::contains(all.content, "ローグライトの周回設計"));
    CHECK(str::contains(all.content, "#1 **ローグライトの周回設計**"));
    const auto draft = route_slash_command(slash("discutere-discussions", std::nullopt, "draft"), d);
    CHECK(str::contains(draft.content, "📝下書き"));
    CHECK(!str::contains(draft.content, "ローグライトの周回設計"));
}

// ─── golden ──────────────────────────────────────────────────────────────

TEST_CASE("golden: routeSlashCommand の返信文が Node と一致") {
    const auto fx = w4q::load_fixture("slash.json");
    w4q::Env env;
    env.kg_exec(fx["kgSetup"]);
    env.flow_exec(fx["flowSetup"]);
    for (const auto& c : fx["slash"]) {
        const auto& in = c["input"];
        std::vector<std::string> admins{"admin1"};
        if (in.contains("adminIds")) admins = in["adminIds"].get<std::vector<std::string>>();
        auto d = deps_with(env, admins);
        InboundSlashCommand cmd;
        cmd.name = in["name"].get<std::string>();
        cmd.argsText = in["argsText"].get<std::string>();
        if (in.contains("userId") && in["userId"].is_string()) cmd.userId = in["userId"].get<std::string>();
        if (in.contains("enabledOption")) cmd.enabledOption = in["enabledOption"].get<bool>();
        cmd.guildId = "G1";
        cmd.channelId = "C1";
        const auto r = route_slash_command(cmd, d);
        const auto expected = w4q::mask_uuids(c["reply"]["content"].get<std::string>());
        if (w4q::mask_uuids(r.content) != expected)
            throw test::Failure(cmd.name + " " + cmd.argsText + "\n  actual:   " + r.content + "\n  expected: " + expected);
        CHECK_EQ(r.ephemeral, c["reply"]["ephemeral"].get<bool>());
    }
}

// ─── command_defs との一致 ───────────────────────────────────────────────

TEST_CASE("command_defs と route の name が一致する (登録だけ / handler だけを防ぐ)") {
    std::set<std::string> defs;
    for (const auto& d : DISCORD_COMMAND_DEFS()) defs.insert(d["name"].get<std::string>());
    const auto in = [](const std::vector<std::string>& v, const std::string& n) { return std::find(v.begin(), v.end(), n) != v.end(); };
    // 登録された全コマンドに handler がある
    for (const auto& name : defs)
        CHECK(in(ROUTER_SLASH_COMMANDS(), name) || in(DEDICATED_SLASH_COMMANDS(), name) || in(DISCATIER_COMMANDS(), name));
    // router / gateway 専用 handler の全コマンドが登録されている
    for (const auto& name : ROUTER_SLASH_COMMANDS()) CHECK(defs.count(name) == 1);
    for (const auto& name : DEDICATED_SLASH_COMMANDS()) CHECK(defs.count(name) == 1);
    for (const auto& name : ADMIN_SLASH_COMMANDS()) CHECK(in(ROUTER_SLASH_COMMANDS(), name));
    // Discatier として登録したものは submit_message が "unknown command" を返さない
    w4q::Env env;
    const auto d = deps_with(env, {});
    for (const auto& name : defs) {
        if (!in(DISCATIER_COMMANDS(), name)) continue;
        const auto r = route_slash_command(slash(name, std::string("u1")), d);
        CHECK(!str::contains(r.content, "unknown command"));
    }
}

TEST_CASE("interaction JSON: argsText (stringifyOptionValue) / enabled / guild・channel の既定") {
    auto cmd = inbound_slash_command_from_json(interaction(
        "propose", Json::array({Json{{"name", "statement"}, {"type", 3}, {"value", "a b"}}, Json{{"name", "n"}, {"type", 4}, {"value", 1.5}},
                                Json{{"name", "e"}, {"type", 5}, {"value", false}}, Json{{"name", "x"}, {"type", 3}, {"value", ""}},
                                Json{{"name", "o"}, {"type", 3}, {"value", Json{{"k", 1}}}}})));
    CHECK_EQ(cmd.argsText, std::string(R"(a b 1.5 false {"k":1})"));
    CHECK(!cmd.enabledOption);
    CHECK_EQ(cmd.guildId, std::string("g1"));
    cmd = inbound_slash_command_from_json(interaction("discutere-kill", Json::array({Json{{"name", "enabled"}, {"type", 5}, {"value", true}}})));
    CHECK(cmd.enabledOption && *cmd.enabledOption);
    // 型違いの enabled は undefined (discord.js の getBoolean が投げる → catch)
    cmd = inbound_slash_command_from_json(interaction("discutere-kill", Json::array({Json{{"name", "enabled"}, {"type", 3}, {"value", "true"}}})));
    CHECK(!cmd.enabledOption);
    // DM (guild 無し・member 無し)
    Json dm = interaction("discutere-queue", Json::array());
    dm.erase("guild_id");
    dm.erase("channel_id");
    dm.erase("member");
    dm["user"] = Json{{"id", "u9"}};
    cmd = inbound_slash_command_from_json(dm);
    CHECK_EQ(cmd.guildId, std::string("dm"));
    CHECK_EQ(cmd.channelId, std::string("default"));
    CHECK_EQ(cmd.userId.value_or(""), std::string("u9"));
    CHECK_THROWS(inbound_slash_command_from_json(Json::array()));
}

DI_TEST_MAIN()
