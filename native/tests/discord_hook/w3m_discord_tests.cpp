// w3m_discord のテスト (1/4)。準備は w3m_discord_support.hpp。
#include "w3m_discord_support.hpp"

using namespace w3m_discord_support;

// ─── command-defs / register-commands ─────────────────────────────

TEST_CASE("command-defs: JSON.stringify(DISCORD_COMMAND_DEFS) と 1 バイトも違わない (golden)") {
    const auto expected = fs::read_file(test::fixtures_dir() + "/w3m/command_defs.json", 1u << 20);
    CHECK(expected.has_value());
    CHECK_EQ(json_stringify(DISCORD_COMMAND_DEFS()), *expected);
    // level-check / mechanics-check の 1 番目は必須の String、3 番目は input (TS テストの観点)
    for (const auto& c : DISCORD_COMMAND_DEFS()) {
        if (c["name"] == "level-check" || c["name"] == "mechanics-check") {
            CHECK_EQ(c["options"][0]["type"], Json(option_type::String));
            CHECK_EQ(c["options"][0]["required"], Json(true));
            CHECK_EQ(c["options"][2]["name"], Json("input"));
        }
    }
}

TEST_CASE("register_slash_commands: guild ごと PUT (dm・空は除く) / guild 無しは global / token・app id 必須") {
    FakeDiscord d;
    auto r = register_slash_commands({kFakeToken, "app-1", {"g1", "dm", "", "g2"}}, d.options());
    CHECK_EQ(r.scope, std::string("guild"));
    CHECK_EQ(r.commandCount, DISCORD_COMMAND_DEFS().size());
    CHECK_EQ(json_stringify(to_json(r)),
             json_stringify(Json{{"scope", "guild"}, {"guildIds", {"g1", "g2"}}, {"commandCount", DISCORD_COMMAND_DEFS().size()}}));
    CHECK_EQ(d.puts.size(), std::size_t(2));
    CHECK_EQ(d.puts[0].first, std::string("/applications/app-1/guilds/g1/commands"));
    CHECK_EQ(d.puts[1].first, std::string("/applications/app-1/guilds/g2/commands"));
    CHECK_EQ(d.puts[0].second, json_stringify(DISCORD_COMMAND_DEFS()));
    r = register_slash_commands({kFakeToken, "app-1", {"dm"}}, d.options());
    CHECK_EQ(r.scope, std::string("global"));
    CHECK(r.guildIds.empty());
    CHECK_EQ(d.puts.back().first, std::string("/applications/app-1/commands"));
    try {
        register_slash_commands({"", "app-1", {}}, d.options());
        CHECK(false);
    } catch (const std::invalid_argument& e) {
        CHECK_EQ(std::string(e.what()), std::string("registerSlashCommands: botToken required"));
    }
    try {
        register_slash_commands({kFakeToken, "", {}}, d.options());
        CHECK(false);
    } catch (const std::invalid_argument& e) {
        CHECK_EQ(std::string(e.what()), std::string("registerSlashCommands: applicationId required"));
    }
    CHECK_EQ(d.puts.size(), std::size_t(3));
}

// ─── level-check / mechanics-check (tool + slash adapter) ────────────

// 1 ケース分の確認。LLM の偽物は make_shared で作ると、Praesidium の静的解析が shared_ptr の参照数の
// 追跡を見失い「解放漏れ」と誤検知する。偽物はこの関数の局所変数に置き、所有しない shared_ptr で渡す。
static void check_diagnostic_tool_case(const Json& c) {
    const auto name = c["name"].get<std::string>();
    std::atomic<int> calls{0};
    const auto text = c["response"].get<std::string>();
    persona_engine::MockLLMClient llm{std::vector<persona_engine::MockResponder>{
        [&text, &calls](const persona_engine::LLMInvokeArgs&) -> Variant {
            ++calls;
            return text;
        }}};
    design_diagnostic::DiagnosticDependencies deps{persona_engine::LLMClientPtr(persona_engine::LLMClientPtr(), &llm), std::nullopt};
    Json result;
    DiagnosticReply reply;
    if (str::starts_with(name, "level")) {
        const auto r = level_check::check_level_consistency(c["input"], deps);
        result = level_check::to_json(r);
        reply = level_check_reply(r);
    } else {
        const auto r = mechanics_check::check_mechanics_consistency(c["input"], deps);
        result = design_diagnostic::to_json(r);
        reply = mechanics_check_reply(r);
    }
    CHECK_EQ(calls.load(), 1);
    CHECK(same(result, parse_json_or_throw(c["result"].get<std::string>())));
    // 返信: 添付かコードブロックか・ファイル名が同じ。中身はキー順を無視して同じ。
    const auto& files = c["reply"]["files"];
    CHECK_EQ(reply.file.has_value(), !files.empty());
    if (reply.file) {
        CHECK_EQ(reply.file->name, files[0]["name"].get<std::string>());
        CHECK(same(parse_json_or_throw(reply.file->data), parse_json_or_throw(files[0]["data"].get<std::string>())));
        CHECK_EQ(reply.content, c["reply"]["content"].get<std::string>());
    } else {
        const auto expected = c["reply"]["content"].get<std::string>();
        const auto strip = [](const std::string& s) { return s.substr(8, s.size() - 12); };  // ```json\n ... \n```
        CHECK(same(parse_json_or_throw(strip(reply.content)), parse_json_or_throw(strip(expected))));
    }
}

TEST_CASE("tool: check_level/mechanics_consistency が Node と同じ結果 (golden、キー順は無視)") {
    for (const auto& c : test::load_fixture("w3m/diagnostic_tools.json")) {
        if (c.contains("input")) check_diagnostic_tool_case(c);
    }
    const auto shortc = fixture_case("w3m/diagnostic_tools.json", "short-reply");
    CHECK_EQ(mechanics_check_reply({}).content, shortc["reply"]["content"].get<std::string>());
}

DI_TEST_MAIN()
