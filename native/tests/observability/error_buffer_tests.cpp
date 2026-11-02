// console-capture + error-buffer + errors-routes (golden: fixtures/w1f/errors.json)。
#include "check.hpp"
#include "w1f_support.hpp"

#include "api/errors_routes.hpp"
#include "base/log.hpp"
#include "base/strings.hpp"
#include "observability/console_capture.hpp"
#include "observability/error_buffer.hpp"

using namespace di;
using namespace di::test::w1f;

namespace {
const Json kAdmin{{"X-User-Id", "golden"}, {"X-User-Role", "admin"}};

void replay(const Json& fx) {
    observability::clear_errors();
    for (const auto& m : fx["messages"]) {
        if (m[0] == "error") log::error(m[1].get<std::string>());
        else log::warn(m[1].get<std::string>());
    }
}
} // namespace

TEST_CASE("console capture records warn/error with the same source inference") {
    log::set_console_enabled(false);
    observability::install_console_capture();
    observability::install_console_capture();  // 二重適用は no-op
    const Json fx = load_fixture("errors.json");
    replay(fx);
    log::info("info は積まない");
    const auto recent = observability::recent_errors();
    CHECK_EQ(recent.size(), fx["recorded"].size());
    for (std::size_t i = 0; i < recent.size(); ++i) {
        CHECK_EQ(recent[i].level, fx["recorded"][i]["level"].get<std::string>());
        CHECK_EQ(recent[i].source, fx["recorded"][i]["source"].get<std::string>());
        CHECK_EQ(recent[i].message, fx["recorded"][i]["message"].get<std::string>());
    }
}

TEST_CASE("errors route: limit parsing and clear match Hono") {
    log::set_console_enabled(false);
    observability::install_console_capture();
    const Json fx = load_fixture("errors.json");
    const auto router = api::errors_routes();
    for (const auto& l : fx["limits"]) {
        replay(fx);
        auto req = make_request("GET", "/admin/errors" + l["query"].get<std::string>(), kAdmin, "127.0.0.1");
        const auto res = router.handle(req);
        CHECK_EQ(res.status, l["status"].get<int>());
        Json messages = Json::array();
        const Json body = body_json(res);
        for (const auto& e : body["errors"]) messages.push_back(e["message"]);
        if (json_stringify(messages) != json_stringify(l["messages"]))
            throw test::Failure("limit " + l["query"].get<std::string>() + ": " + json_stringify(messages) + " body " + res.body.substr(0, 300));
    }
    auto clear = make_request("POST", "/admin/errors/clear", kAdmin, "127.0.0.1");
    const auto res = router.handle(clear);
    CHECK_EQ(res.status, fx["clear"]["status"].get<int>());
    CHECK_EQ(res.body, json_stringify(fx["clear"]["json"]));
    CHECK(observability::recent_errors().empty());
}

TEST_CASE("buffer keeps the newest 200 (cap exact / cap+1)") {
    observability::clear_errors();
    for (int i = 0; i < 200; ++i) observability::record_error("warn", "t", std::to_string(i));
    CHECK_EQ(observability::recent_errors().size(), 200u);
    CHECK_EQ(observability::recent_errors().back().message, "0");
    observability::record_error("error", "t", "200");
    const auto all = observability::recent_errors();
    CHECK_EQ(all.size(), 200u);
    CHECK_EQ(all.front().message, "200");
    CHECK_EQ(all.back().message, "1");
    CHECK_EQ(observability::recent_errors(0).size(), 0u);
    CHECK_EQ(observability::recent_errors(-3).size(), 0u);
    CHECK_EQ(observability::recent_errors(1e18).size(), 200u);
    const Json j = observability::to_json(all.front());
    CHECK_EQ(json_stringify(Json{{"level", j["level"]}, {"source", j["source"]}, {"message", j["message"]}}),
             R"({"level":"error","source":"t","message":"200"})");
    CHECK(j["ts"].is_number_integer());
}

TEST_CASE("message length limit: exactly the cap is kept, cap+1 is cut, huge input is bounded") {
    observability::clear_errors();
    const std::string exact(observability::kErrorMessageMax, 'a');
    observability::record_error("error", "t", exact);
    CHECK_EQ(observability::recent_errors(1)[0].message.size(), exact.size());
    observability::record_error("error", "t", exact + "b");
    CHECK_EQ(observability::recent_errors(1)[0].message, exact);
    observability::record_error("error", "t", std::string(8u << 20, 'x'));
    CHECK_EQ(observability::recent_errors(1)[0].message.size(), observability::kErrorMessageMax);
    // 多バイト文字の途中で切らない (UTF-16 単位で数える)。
    observability::record_error("error", "t", str::repeat("あ", observability::kErrorMessageMax + 3));
    CHECK_EQ(str::js_length(observability::recent_errors(1)[0].message), observability::kErrorMessageMax);
    CHECK(str::is_valid_utf8(observability::recent_errors(1)[0].message));
}

TEST_CASE("infer_source edge cases") {
    CHECK_EQ(observability::infer_source("[x]"), "x");
    CHECK_EQ(observability::infer_source("[]"), "console");
    CHECK_EQ(observability::infer_source("[unterminated"), "console");
    CHECK_EQ(observability::infer_source("abc"), "console");
    CHECK_EQ(observability::infer_source("abc:"), "abc");
    CHECK_EQ(observability::infer_source(std::string(1u << 20, 'a') + ":"), std::string(1u << 20, 'a'));
}

DI_TEST_MAIN()
