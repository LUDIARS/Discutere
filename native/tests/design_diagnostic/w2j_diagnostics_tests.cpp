// w2-J: design-diagnostic (documents / response / invoke)・level-check・mechanics-check を Node の golden と照合する。
// tests/level-check/{request,play-envelope,tool}.test.ts、tests/mechanics-check/{request,tool}.test.ts の観点。
#include <atomic>
#include <chrono>
#include <thread>

#include "check.hpp"
#include "core/test_util.hpp"

#include "base/strings.hpp"
#include "design_diagnostic/invoke.hpp"
#include "design_diagnostic/response.hpp"
#include "level_check/play_envelope.hpp"
#include "level_check/play_findings.hpp"
#include "level_check/prompt.hpp"
#include "level_check/request.hpp"
#include "level_check/response.hpp"
#include "mechanics_check/prompt.hpp"
#include "mechanics_check/request.hpp"
#include "mechanics_check/response.hpp"
#include "persona_engine/llm/mock.hpp"

using namespace di;

namespace {

/** キー順を無視して比べる (TS は kind を末尾に置くことがある。port-notes 参照)。 */
bool same(const Json& a, const Json& b) { return nlohmann::json::parse(a.dump()) == nlohmann::json::parse(b.dump()); }

template <class Fn>
Json run(Fn&& fn) {
    try {
        return Json{{"ok", true}, {"value", parse_json_or_throw(json_stringify(fn()), {64u << 20, 5'000'000, 64})}};
    } catch (const design_diagnostic::DiagnosticError& e) {
        return Json{{"ok", false}, {"code", e.code()}, {"error", e.what()}};
    } catch (const std::exception& e) {
        return Json{{"ok", false}, {"code", nullptr}, {"error", e.what()}};
    }
}

void expect_same(const Json& actual, const Json& expected, const std::string& name) {
    if (!same(actual, expected)) throw test::Failure(name + "\n  actual:   " + actual.dump().substr(0, 2000) + "\n  expected: " + expected.dump().substr(0, 2000));
}

Json envelopes_json(const std::vector<level_check::PlayEnvelope>& es) {
    Json a = Json::array();
    for (const auto& e : es) a.push_back(level_check::to_json(e));
    return a;
}

Json findings_json(const level_check::PlayFindings& f) {
    Json d = Json::array();
    for (const auto& x : f.differences) d.push_back(level_check::to_json(x));
    Json g = Json::array();
    for (const auto& x : f.gaps) g.push_back(design_diagnostic::to_json(x));
    return Json{{"differences", d}, {"gaps", g}};
}

Json level_request_json(const level_check::LevelCheckRequest& r) {
    Json j = Json::object();
    j["levelText"] = r.levelText;
    if (r.baselineText) j["baselineText"] = *r.baselineText;
    Json refs = Json::array();
    for (const auto& x : r.references.value_or(std::vector<level_check::LevelReference>{})) refs.push_back(design_diagnostic::to_json(x));
    j["references"] = refs;
    if (r.playModel) j["playModel"] = level_check::to_json(*r.playModel);
    return j;
}

Json mech_request_json(const mechanics_check::MechanicsCheckRequest& r) {
    Json j = Json::object();
    j["specText"] = r.specText;
    if (r.baselineText) j["baselineText"] = *r.baselineText;
    Json refs = Json::array();
    for (const auto& x : r.references.value_or(std::vector<mechanics_check::MechanicsReference>{})) refs.push_back(design_diagnostic::to_json(x));
    j["references"] = refs;
    return j;
}

} // namespace

TEST_CASE("level-check: 要求の検証・プロンプト・到達幅・所見 (golden)") {
    for (const auto& c : test::load_fixture("w2j/level_requests.json")) {
        const std::string name = c["name"].get<std::string>();
        expect_same(run([&] { return level_request_json(level_check::validate_level_check_request(c["input"])); }), c["validated"], name + " validated");
        if (!c["validated"]["ok"].get<bool>()) {
            CHECK_EQ(run([&] { return Json(level_request_json(level_check::validate_level_check_request(c["input"]))); })["code"].get<std::string>(),
                     std::string("invalid_input"));
            continue;
        }
        const auto req = level_check::validate_level_check_request(c["input"]);
        // プロンプトは 1 バイト単位で一致すること (LLM への入力・引用の照合元)。
        const auto p = level_check::build_level_check_prompt(req);
        CHECK_EQ(p.system, c["prompt"]["value"]["system"].get<std::string>());
        CHECK_EQ(p.prompt, c["prompt"]["value"]["prompt"].get<std::string>());
        expect_same(run([&] { return envelopes_json(req.playModel ? level_check::calculate_play_envelopes(*req.playModel) : std::vector<level_check::PlayEnvelope>{}); }),
                    c["envelopes"], name + " envelopes");
        expect_same(run([&] {
                        return findings_json(level_check::compose_play_findings(
                            req, req.playModel ? level_check::calculate_play_envelopes(*req.playModel) : std::vector<level_check::PlayEnvelope>{}));
                    }),
                    c["findings"], name + " findings");
    }
}

TEST_CASE("level-check: LLM 応答の検証 (golden)") {
    for (const auto& c : test::load_fixture("w2j/level_responses.json")) {
        const auto req = level_check::validate_level_check_request(c["request"]);
        expect_same(run([&] { return design_diagnostic::to_json(level_check::parse_level_check_response(c["text"].get<std::string>(), req)); }),
                    c["result"], c["name"].get<std::string>());
    }
}

TEST_CASE("mechanics-check: 要求・プロンプト・応答 (golden)") {
    for (const auto& c : test::load_fixture("w2j/mechanics_requests.json")) {
        const std::string name = c["name"].get<std::string>();
        expect_same(run([&] { return mech_request_json(mechanics_check::validate_mechanics_check_request(c["input"])); }), c["validated"], name);
        if (!c["validated"]["ok"].get<bool>()) continue;
        const auto p = mechanics_check::build_mechanics_check_prompt(mechanics_check::validate_mechanics_check_request(c["input"]));
        CHECK_EQ(p.system, c["prompt"]["value"]["system"].get<std::string>());
        CHECK_EQ(p.prompt, c["prompt"]["value"]["prompt"].get<std::string>());
    }
    for (const auto& c : test::load_fixture("w2j/mechanics_responses.json")) {
        const auto req = mechanics_check::validate_mechanics_check_request(c["request"]);
        expect_same(run([&] { return design_diagnostic::to_json(mechanics_check::parse_mechanics_check_response(c["text"].get<std::string>(), req)); }),
                    c["result"], c["name"].get<std::string>());
    }
}

TEST_CASE("response: 応答の大きさの上限ちょうど/+1・巨大・深い入れ子") {
    const auto req = mechanics_check::validate_mechanics_check_request(Json{{"specText", "spec"}});
    const std::size_t limit = design_diagnostic::DIAGNOSTIC_LIMITS.responseBytes;
    // ちょうど上限の大きさの空白つき JSON は JSON として読む (中身の検証で落ちる)
    std::string exact = "{\"logic_differences\":[],\"design_gap\":[]}";
    exact += std::string(limit - exact.size(), ' ');
    auto r = run([&] { return design_diagnostic::to_json(mechanics_check::parse_mechanics_check_response(exact, req)); });
    CHECK(str::contains(r["error"].get<std::string>(), "empty diagnostic"));
    r = run([&] { return design_diagnostic::to_json(mechanics_check::parse_mechanics_check_response(exact + " ", req)); });
    CHECK(str::contains(r["error"].get<std::string>(), "byte limit"));
    r = run([&] { return design_diagnostic::to_json(mechanics_check::parse_mechanics_check_response(std::string(50u << 20, '['), req)); });
    CHECK_EQ(r["code"].get<std::string>(), std::string("invalid_response"));
    r = run([&] { return design_diagnostic::to_json(mechanics_check::parse_mechanics_check_response(str::repeat("[", 5000) + str::repeat("]", 5000), req)); });
    CHECK(str::contains(r["error"].get<std::string>(), "expected JSON only"));
}

TEST_CASE("invoke: conversationOnly・期限・失敗の文言に秘密を含めない・同時実行の上限") {
    using namespace persona_engine;
    auto ok = std::make_shared<MockLLMClient>(std::vector<MockResponder>{[](const LLMInvokeArgs&) { return std::variant<std::string, LLMResult>(std::string("{}")); }});
    CHECK_EQ(design_diagnostic::invoke_diagnostic({"sys", "p"}, {ok, std::string("m")}), std::string("{}"));
    const auto last = ok->last_invocation();
    CHECK(last && last->conversationOnly && last->timeoutMs == 60'000 && last->maxTokens == 8'000 && last->model == std::string("m"));

    auto expect_code = [](const std::function<void()>& fn, const std::string& code) {
        try {
            fn();
            CHECK(false);
        } catch (const design_diagnostic::DiagnosticError& e) {
            CHECK_EQ(e.code(), code);
            CHECK(!str::contains(e.what(), "secret"));
        }
    };
    expect_code([] { design_diagnostic::invoke_diagnostic({"s", "p"}, {nullptr, std::nullopt}); }, "llm_unavailable");
    auto failing = std::make_shared<MockLLMClient>(
        std::vector<MockResponder>{[](const LLMInvokeArgs&) { return std::variant<std::string, LLMResult>(LLMResult::failure("secret-backend-error")); }});
    expect_code([&] { design_diagnostic::invoke_diagnostic({"s", "p"}, {failing, std::nullopt}); }, "llm_failed");
    auto throwing = std::make_shared<MockLLMClient>(
        std::vector<MockResponder>{[](const LLMInvokeArgs&) -> std::variant<std::string, LLMResult> { throw std::runtime_error("secret-backend-error"); }});
    expect_code([&] { design_diagnostic::invoke_diagnostic({"s", "p"}, {throwing, std::nullopt}); }, "llm_failed");

    std::atomic<bool> release{false};
    auto slow = std::make_shared<MockLLMClient>(std::vector<MockResponder>(8, [&](const LLMInvokeArgs&) {
        while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return std::variant<std::string, LLMResult>(std::string("{}"));
    }));
    for (int i = 0; i < design_diagnostic::kMaxConcurrentDiagnostics; ++i)
        expect_code([&] { design_diagnostic::invoke_diagnostic({"s", "p"}, {slow, std::nullopt}, std::chrono::milliseconds(20)); }, "llm_timeout");
    // 打ち切った呼び出しがまだ走っている間は上限で断る
    expect_code([&] { design_diagnostic::invoke_diagnostic({"s", "p"}, {slow, std::nullopt}, std::chrono::milliseconds(20)); }, "llm_unavailable");
    release = true;
    for (int i = 0; i < 200; ++i) {
        try {
            design_diagnostic::invoke_diagnostic({"s", "p"}, {ok, std::nullopt});
            break;
        } catch (const design_diagnostic::DiagnosticError&) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
}

DI_TEST_MAIN()
