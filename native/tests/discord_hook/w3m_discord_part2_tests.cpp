// w3m_discord のテスト (2/4)。準備は w3m_discord_support.hpp。
#include "w3m_discord_support.hpp"

using namespace w3m_discord_support;

TEST_CASE("tool: 検証失敗は LLM を呼ばない・LLM 失敗は秘密を出さない・壊れた応答は invalid_response・期限") {
    const auto lreq = fixture_case("w2j/level_requests.json", "base")["input"];
    const auto lres = fixture_case("w2j/level_responses.json", "valid")["text"].get<std::string>();
    std::atomic<int> calls{0};
    design_diagnostic::DiagnosticDependencies deps{llm_text(lres, &calls), std::nullopt};
    try {
        level_check::check_level_consistency(Json{{"levelText", ""}}, deps);
        CHECK(false);
    } catch (const design_diagnostic::DiagnosticError& e) {
        CHECK(str::contains(e.what(), "levelText"));
    }
    CHECK_EQ(calls.load(), 0);
    auto mreq = fixture_case("w2j/mechanics_requests.json", "base")["input"];
    mreq["playModel"] = Json::object();
    try {
        mechanics_check::check_mechanics_consistency(mreq, deps);
        CHECK(false);
    } catch (const design_diagnostic::DiagnosticError& e) {
        CHECK_EQ(e.code(), std::string("use_level_check"));
    }
    CHECK_EQ(calls.load(), 0);
    auto failing = std::make_shared<persona_engine::MockLLMClient>(std::vector<persona_engine::MockResponder>{
        [](const persona_engine::LLMInvokeArgs&) -> Variant { return persona_engine::LLMResult::failure("secret-backend-error"); }});
    try {
        level_check::check_level_consistency(lreq, {failing, std::nullopt});
        CHECK(false);
    } catch (const design_diagnostic::DiagnosticError& e) {
        CHECK_EQ(e.code(), std::string("llm_failed"));
        CHECK(!str::contains(e.what(), "secret"));
    }
    try {
        level_check::check_level_consistency(lreq, {llm_text("{}"), std::nullopt});
        CHECK(false);
    } catch (const design_diagnostic::DiagnosticError& e) {
        CHECK_EQ(e.code(), std::string("invalid_response"));
    }
    auto slow = std::make_shared<persona_engine::MockLLMClient>(std::vector<persona_engine::MockResponder>{
        [](const persona_engine::LLMInvokeArgs&) -> Variant {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            return std::string("late");
        }});
    try {
        level_check::check_level_consistency(lreq, {slow, std::nullopt}, std::chrono::milliseconds(20));
        CHECK(false);
    } catch (const design_diagnostic::DiagnosticError& e) {
        CHECK_EQ(e.code(), std::string("llm_timeout"));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(350));  // 打ち切った呼び出しの終わりを待つ
}

TEST_CASE("slash input: 不正な JSON・配列・余計なキー・Discord 上限 (ちょうど/+1)・playModel の扱い") {
    const auto lreq = fixture_case("w2j/level_requests.json", "base")["input"];
    const Json extra{{"references", lreq["references"]}, {"playModel", lreq["playModel"]}};
    const auto parsed = read_level_check_slash_input(lreq["levelText"].get<std::string>(), lreq["baselineText"].get<std::string>(),
                                                     json_stringify(extra));
    CHECK(parsed.playModel.has_value());
    CHECK(same(level_check::to_json(*parsed.playModel), lreq["playModel"]));
    for (const std::string& bad : {std::string("[]"), std::string("{}junk"), std::string(R"({"levelText":"override"})"),
                                  std::string(6'001, 'x')})
        CHECK_THROWS(read_level_check_slash_input("spec", std::nullopt, bad));
    // ちょうど 6000 文字 (UTF-16) は長さでは拒否しない (JSON として読めれば通る)
    const std::string exact = "{\"references\":[]" + std::string(6'000 - 17, ' ') + "}";
    CHECK_EQ(str::js_length(exact), std::size_t(6'000));
    CHECK_EQ(read_level_check_slash_input("spec", std::nullopt, exact).levelText, std::string("spec"));
    // 全角 3000 字 (UTF-16 で 3000) は 6000 バイト超でも長さ内
    const auto mreq = fixture_case("w2j/mechanics_requests.json", "base")["input"];
    const auto m = read_mechanics_check_slash_input(mreq["specText"].get<std::string>(), mreq["baselineText"].get<std::string>(),
                                                    json_stringify(Json{{"references", mreq["references"]}}));
    CHECK(same(design_diagnostic::to_json(m.references->at(0)), mreq["references"][0]));
    try {
        read_mechanics_check_slash_input("spec", std::nullopt, R"({"playModel":{}})");
        CHECK(false);
    } catch (const design_diagnostic::DiagnosticError& e) {
        CHECK_EQ(e.code(), std::string("use_level_check"));
    }
    for (const std::string& bad : {std::string("[]"), std::string("{}junk"), std::string(R"({"specText":"override"})"), std::string(6'001, 'x'),
                                  str::repeat("[", 200) + str::repeat("]", 200)})
        CHECK_THROWS(read_mechanics_check_slash_input("spec", std::nullopt, bad));
}

TEST_CASE("handle_*_command: defer → LLM → edit を 1 回ずつ、失敗は診断失敗の返信、他コマンドは false") {
    const auto lreq = fixture_case("w2j/level_requests.json", "base")["input"];
    const auto lres = fixture_case("w2j/level_responses.json", "valid")["text"].get<std::string>();
    const auto input = json_stringify(Json{{"references", lreq["references"]}, {"playModel", lreq["playModel"]}});
    auto inter = slash("level-check", {{"level", lreq["levelText"].get<std::string>()},
                                       {"baseline", lreq["baselineText"].get<std::string>()},
                                       {"input", input}});
    std::vector<std::string> lifecycle;
    RecordingResponder rec(&lifecycle);
    auto mock = std::make_shared<persona_engine::MockLLMClient>(std::vector<persona_engine::MockResponder>{
        [&](const persona_engine::LLMInvokeArgs&) -> Variant {
            lifecycle.push_back("llm");
            return lres;
        }});
    design_diagnostic::DiagnosticDependencies deps{mock, std::nullopt};
    CHECK(handle_level_check_command(inter, rec, &deps));
    CHECK_EQ(str::join(lifecycle, ","), std::string("defer,llm,edit"));
    CHECK_EQ(rec.replies.size(), std::size_t(1));
    CHECK(rec.replies[0].file.has_value());
    CHECK_EQ(rec.replies[0].file->name, std::string("level-check.json"));
    const auto attached = parse_json_or_throw(rec.replies[0].file->data);
    std::vector<std::string> keys;
    for (auto it = attached.begin(); it != attached.end(); ++it) keys.push_back(it.key());
    CHECK_EQ(str::join(keys, ","), std::string("logic_differences,design_gap"));
    CHECK_EQ(json_stringify(rec.replies[0].body()["allowed_mentions"]), std::string(R"({"parse":[]})"));

    // LLM 無し / 失敗 (秘密を出さない) / 壊れた応答 → defer, edit のみ・診断失敗
    auto secret = std::make_shared<persona_engine::MockLLMClient>(std::vector<persona_engine::MockResponder>{
        [](const persona_engine::LLMInvokeArgs&) -> Variant { return persona_engine::LLMResult::failure("secret-token"); }});
    design_diagnostic::DiagnosticDependencies d_secret{secret, std::nullopt};
    design_diagnostic::DiagnosticDependencies d_bad{llm_text("bad JSON"), std::nullopt};
    const std::vector<const design_diagnostic::DiagnosticDependencies*> variants{nullptr, &d_secret, &d_bad};
    for (const auto* deps2 : variants) {
        lifecycle.clear();
        RecordingResponder r2(&lifecycle);
        CHECK(handle_level_check_command(inter, r2, deps2));
        CHECK_EQ(str::join(lifecycle, ","), std::string("defer,edit"));
        CHECK(str::contains(r2.replies[0].content, "診断失敗"));
        CHECK(!str::contains(r2.replies[0].content, "secret-token"));
        CHECK(!r2.replies[0].file.has_value());
    }
    // 他のコマンドは触らない
    lifecycle.clear();
    RecordingResponder r3(&lifecycle);
    CHECK(!handle_level_check_command(slash("another-command", {}), r3, &deps));
    CHECK(!handle_mechanics_check_command(inter, r3, &deps));
    CHECK(lifecycle.empty());
    // editReply の失敗は呼び出し側へ。defer の失敗では LLM を呼ばない
    std::atomic<int> calls{0};
    design_diagnostic::DiagnosticDependencies counted{llm_text(lres, &calls), std::nullopt};
    RecordingResponder fe(&lifecycle);
    fe.fail_edit = true;
    CHECK_THROWS(handle_level_check_command(inter, fe, &counted));
    CHECK_EQ(calls.load(), 1);
    RecordingResponder fd(&lifecycle);
    fd.fail_defer = true;
    CHECK_THROWS(handle_level_check_command(inter, fd, &counted));
    CHECK_EQ(calls.load(), 1);

    // mechanics: playModel 入力は LLM を呼ばずに /level-check を案内
    const auto mreq = fixture_case("w2j/mechanics_requests.json", "base")["input"];
    std::atomic<int> mcalls{0};
    design_diagnostic::DiagnosticDependencies md{llm_text("{}", &mcalls), std::nullopt};
    lifecycle.clear();
    RecordingResponder r4(&lifecycle);
    CHECK(handle_mechanics_check_command(slash("mechanics-check", {{"spec", mreq["specText"].get<std::string>()}, {"input", R"({"playModel":{}})"}}),
                                         r4, &md));
    CHECK_EQ(mcalls.load(), 0);
    CHECK(str::contains(r4.replies[0].content, "/level-check"));
}

DI_TEST_MAIN()
