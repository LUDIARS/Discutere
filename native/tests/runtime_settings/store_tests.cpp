// runtime-settings/store + api/tuning-routes。
// tests/runtime-settings/runtime-settings.test.ts と tests/secrets/tuning-secrets.test.ts の移植 +
// golden (fixtures/w1f/runtime_settings.json: merge 関数と tuning-routes の要求→応答の並び)。
#include "check.hpp"
#include "w1f_support.hpp"

#include <cmath>

#include "api/tuning_routes.hpp"
#include "base/env.hpp"
#include "base/strings.hpp"
#include "runtime_settings/js_value.hpp"
#include "runtime_settings/store.hpp"

using namespace di;
using namespace di::test::w1f;
namespace rs = di::runtime_settings;

namespace {

rs::FacilitatorTuning def_tuning() {
    rs::FacilitatorTuning t;
    t.tickMs = 30000;
    t.idleGapMs = 120000;
    t.maxPersonas = 20;
    t.aufhebungTarget = 3;
    t.convergePolicy = "default";
    return t;
}

rs::CostRelaySettings def_cost() {
    rs::CostRelaySettings c;
    c.service = "discutere";
    return c;
}

rs::RuntimeSettingsDefaults golden_defaults() {
    rs::RuntimeSettingsDefaults d;
    d.facilitator = def_tuning();
    d.rolePrompts = {{"ファシリテーター", "司会の既定"}, {"意見屋", "意見屋の既定"}, {"5", "数字キー"}};
    d.ruleInstructions = {{"tick-con-opus", "否定の既定"}, {"tick-pro", "肯定の既定"}};
    d.costRelay = def_cost();
    return d;
}

Json run_step(const http::Router& router, const Json& step) {
    const bool has_body = step.contains("body");
    Json headers = Json::object();
    if (has_body) headers["content-type"] = "application/json";
    auto req = make_request(step["method"].get<std::string>(), step["path"].get<std::string>(), headers, "127.0.0.1",
                            has_body ? step["body"].get<std::string>() : "");
    const auto res = router.handle(req);
    Json out = Json::object();
    out["status"] = res.status;
    auto parsed = parse_json(res.body);
    out["json"] = parsed ? *parsed : Json(nullptr);
    out["text"] = parsed ? Json(nullptr) : Json(res.body);
    return out;
}

void check_step(const http::Router& router, const Json& step) {
    const Json got = run_step(router, step);
    const std::string label = step["method"].get<std::string>() + " " + step["path"].get<std::string>() + " " +
                              (step.contains("body") ? step["body"].get<std::string>() : "");
    if (got["status"] != step["status"]) throw test::Failure(label + " status " + json_stringify(got["status"]));
    if (json_stringify(got["json"]) != json_stringify(step["json"]))
        throw test::Failure(label + "\n  json " + json_stringify(got["json"]) + "\n  want " + json_stringify(step["json"]));
    if (json_stringify(got["text"]) != json_stringify(step["text"])) throw test::Failure(label + " text " + json_stringify(got["text"]));
}

} // namespace

TEST_CASE("merge_facilitator_tuning / merge_cost_relay match Node for odd overrides") {
    const Json fx = load_fixture("runtime_settings.json");
    for (const auto& c : fx["fac"])
        CHECK_EQ(json_stringify(rs::to_json(rs::merge_facilitator_tuning(def_tuning(), c["override"]))), json_stringify(c["result"]));
    for (const auto& c : fx["cr"])
        CHECK_EQ(json_stringify(rs::to_json(rs::merge_cost_relay(def_cost(), c["override"]))), json_stringify(c["result"]));
    // NaN (JSON に書けない値) も既定に落ちる
    CHECK_EQ(rs::merge_facilitator_tuning(def_tuning(), Json{{"maxPersonas", std::nan("")}}).maxPersonas, 20.0);
}

TEST_CASE("tuning routes: request sequence matches Hono (503 before injection, 400/500/404 edges)") {
    env::set_for_test("DISCUTERE_YOUTUBE_API_KEY", std::nullopt);
    const Json fx = load_fixture("runtime_settings.json");
    const auto router = api::tuning_routes();
    api::set_runtime_settings(nullptr);
    for (const auto& s : fx["before"]) check_step(router, s);

    auto db = sql::Database::open(":memory:");
    api::set_runtime_settings(rs::create_runtime_settings_store(db, golden_defaults()));
    int reseeds = 0;
    api::set_rule_reseeder([&] { ++reseeds; });
    for (const auto& s : fx["after"]) check_step(router, s);
    CHECK_EQ(reseeds, fx["reseeds"].get<int>());
    Json rows = Json::array();
    for (const auto& r : db->all("SELECT key, value FROM runtime_settings ORDER BY key")) rows.push_back(r.to_json());
    CHECK_EQ(json_stringify(rows), json_stringify(fx["rows"]));
    api::set_runtime_settings(nullptr);
    api::set_rule_reseeder(nullptr);
}

TEST_CASE("store: facilitator / cost relay persist, role and rule override / reset (runtime-settings.test.ts)") {
    auto db = sql::Database::open(":memory:");
    rs::RuntimeSettingsDefaults d;
    d.facilitator = def_tuning();
    d.rolePrompts = {{"ファシリテーター", "司会の既定"}, {"意見屋", "意見屋の既定"}};
    d.ruleInstructions = {{"tick-con-opus", "否定の既定"}};
    d.costRelay = def_cost();
    auto store = rs::create_runtime_settings_store(db, d);
    CHECK(store->get_facilitator_tuning() == def_tuning());
    const auto eff = store->set_facilitator_tuning(Json{{"maxPersonas", 12}, {"convergePolicy", "aufhebung-only"}});
    CHECK_EQ(eff.maxPersonas, 12.0);
    CHECK_EQ(eff.convergePolicy, "aufhebung-only");
    CHECK_EQ(store->get_facilitator_tuning().idleGapMs, 120000.0);

    rs::RuntimeSettingsDefaults empty;
    empty.facilitator = def_tuning();
    empty.costRelay = def_cost();
    auto store2 = rs::create_runtime_settings_store(db, empty);
    CHECK_EQ(store2->get_facilitator_tuning().maxPersonas, 12.0);

    CHECK(store->get_cost_relay() == def_cost());
    const auto cr = store->set_cost_relay(Json{{"enabled", true}, {"concordiaUrl", "http://localhost:17330"}});
    CHECK(cr.enabled);
    CHECK_EQ(cr.concordiaUrl, "http://localhost:17330");
    CHECK_EQ(cr.anatomiaUrl, "");
    const auto cr2 = store->set_cost_relay(Json{{"anatomiaUrl", "http://localhost:4200"}});
    CHECK(cr2.enabled);
    CHECK_EQ(cr2.concordiaUrl, "http://localhost:17330");
    auto store3 = rs::create_runtime_settings_store(db, empty);
    CHECK_EQ(store3->get_cost_relay().anatomiaUrl, "http://localhost:4200");

    CHECK_EQ(store->get_role_prompt("ファシリテーター"), "司会の既定");
    store->set_role_prompt("ファシリテーター", std::string("新しい司会指示"));
    CHECK_EQ(store->get_role_prompt("ファシリテーター"), "新しい司会指示");
    const auto views = store->list_role_prompts();
    CHECK_EQ(views[0].key, "rolePrompt:ファシリテーター");
    CHECK_EQ(views[0].override_value.value(), "新しい司会指示");
    CHECK_EQ(views[0].default_value, "司会の既定");
    store->set_role_prompt("ファシリテーター", std::string(""));
    CHECK_EQ(store->get_role_prompt("ファシリテーター"), "司会の既定");
    CHECK_EQ(store->get_role_prompt("未知"), "");

    CHECK_EQ(store->get_rule_instruction("tick-con-opus"), "否定の既定");
    store->set_rule_instruction("tick-con-opus", std::string("もっと鋭く反論して"));
    CHECK_EQ(store->get_rule_instruction("tick-con-opus"), "もっと鋭く反論して");
    store->set_rule_instruction("tick-con-opus", std::nullopt);
    CHECK_EQ(store->get_rule_instruction("tick-con-opus"), "否定の既定");

    // reload は別経路で書かれた値を読み直す
    db->run("INSERT INTO runtime_settings (key, value, updated_at) VALUES ('rolePrompt:意見屋', '外から', 0)");
    CHECK_EQ(store->get_role_prompt("意見屋"), "意見屋の既定");
    store->reload();
    CHECK_EQ(store->get_role_prompt("意見屋"), "外から");
}

TEST_CASE("broken / oversized stored override falls back to defaults") {
    auto db = sql::Database::open(":memory:");
    db->exec("CREATE TABLE runtime_settings (key TEXT PRIMARY KEY, value TEXT NOT NULL, updated_at INTEGER NOT NULL)");
    db->run("INSERT INTO runtime_settings VALUES ('facilitator', '{broken', 0)");
    db->run("INSERT INTO runtime_settings VALUES ('costRelay', ?, 0)", {std::string(300 * 1024, ' ') + "{}"});
    rs::RuntimeSettingsDefaults d;
    d.facilitator = def_tuning();
    d.costRelay = def_cost();
    auto store = rs::create_runtime_settings_store(db, d);
    CHECK(store->get_facilitator_tuning() == def_tuning());
    CHECK(store->get_cost_relay() == def_cost());
    // 深すぎる入れ子も上限で弾く
    db->run("UPDATE runtime_settings SET value = ? WHERE key = 'facilitator'", {str::repeat("[", 100) + str::repeat("]", 100)});
    store->reload();
    CHECK(store->get_facilitator_tuning() == def_tuning());
    // 壊れた override に部分更新を重ねても既定 + patch になる
    const auto eff = store->set_facilitator_tuning(Json{{"maxPersonas", 7}});
    CHECK_EQ(eff.maxPersonas, 7.0);
    CHECK_EQ(eff.tickMs, 30000.0);
}

TEST_CASE("tuning body edges: invalid / huge / deep bodies behave like {} and the UI has no key input") {
    auto db = sql::Database::open(":memory:");
    rs::RuntimeSettingsDefaults d;
    d.facilitator = def_tuning();
    d.costRelay = def_cost();
    api::set_runtime_settings(rs::create_runtime_settings_store(db, d));
    const auto router = api::tuning_routes();
    for (const std::string& body : {std::string("{"), str::repeat("[", 200) + str::repeat("]", 200),
                                    "{\"maxPersonas\": " + std::string(2u << 20, '1') + "}", std::string("\"text\""), std::string("5")}) {
        auto req = make_request("PUT", "/admin/tuning/facilitator", Json::object(), "127.0.0.1", body);
        const auto res = router.handle(req);
        CHECK_EQ(res.status, 200);
        CHECK_EQ(body_json(res)["facilitator"]["maxPersonas"], 20);
    }
    CHECK(router.max_body_for("PUT", "/admin/tuning/facilitator") == http::kDefaultMaxBody);

    auto page = make_request("GET", "/admin/tuning");
    const auto res = router.handle(page);
    CHECK_EQ(res.status, 200);
    CHECK(str::contains(res.body, "Excubitor Vault"));
    for (const char* forbidden : {"yt_apiKey", "saveYoutubeKey", "refreshYoutubeKey", "importYoutubeKeyFromGcloud"})
        CHECK(!str::contains(res.body, forbidden));
    for (const char* path : {"/admin/tuning/secrets/youtube-api-key", "/admin/tuning/secrets/youtube-api-key/refresh",
                             "/admin/tuning/secrets/youtube-api-key/gcloud"}) {
        auto put = make_request("PUT", path, Json::object(), "127.0.0.1", R"({"apiKey":"test-key"})");
        CHECK_EQ(router.handle(put).status, 404);
        auto post = make_request("POST", path, Json::object(), "127.0.0.1", R"({"apiKey":"test-key"})");
        CHECK_EQ(router.handle(post).status, 404);
    }
    api::set_runtime_settings(nullptr);
}

TEST_CASE("js value coercion") {
    namespace js = rs::js;
    CHECK_EQ(js::to_string(Json::parse("[1,null,\"a\",[2,3]]")), "1,,a,2,3");
    CHECK_EQ(js::to_string(Json::parse("{\"a\":1}")), "[object Object]");
    CHECK_EQ(js::to_string(Json(1.5)), "1.5");
    CHECK_EQ(js::to_number(Json("  0x1f ")), 31.0);
    CHECK(std::isnan(js::to_number(Json::object())));
    CHECK_EQ(js::to_number(Json::array()), 0.0);
    CHECK_EQ(js::round(-2.5), -2.0);
    CHECK_EQ(js::round(2.5), 3.0);
}

DI_TEST_MAIN()
