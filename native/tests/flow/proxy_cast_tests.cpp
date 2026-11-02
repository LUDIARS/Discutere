// w2-H: src/flow/proxy/{bundle,store,cast,resolve,stance}.ts の移植の確認。
// tests/flow/proxy-debate.test.ts の 束・取込 (store)・立場・指定の読み取りとキャスト の観点 + Node の golden。
#include "w2h_support.hpp"

#include <chrono>

#include "base/strings.hpp"
#include "config.hpp"
#include "flow/proxy/bundle.hpp"
#include "flow/proxy/cast.hpp"
#include "flow/proxy/resolve.hpp"
#include "flow/proxy/stance.hpp"
#include "flow/proxy/store.hpp"
#include "persona_engine/llm/mock.hpp"

using namespace di;
using namespace di::flow;
using namespace di::flow::proxy;

namespace {

const double NOW = 1'800'000'000'000.0;
const double DAY = 86'400'000.0;

PersonaPack pack_of(const Json& raw) {
    auto v = parse_persona_pack(raw);
    if (!v.ok) throw test::Failure("pack invalid: " + json_stringify(to_json(v)));
    return *v.pack;
}

Json bundle_json(const std::optional<ProxyBundle>& b) { return b ? to_json(*b) : Json(nullptr); }

Json request_json(const std::optional<ProxyRequest>& r) { return r ? to_json(*r) : Json(nullptr); }

/** golden のパックの speakerKey / styleNotes を差し替えたもの。 */
Json pack_with(const std::string& key_suffix, const Json& patch = Json::object()) {
    Json p = w2h::fx()["bundle"]["pack"];
    std::string suffix = key_suffix;
    while (suffix.size() < 16) suffix = "0" + suffix;
    p["speakerKey"] = "ext:steam:" + suffix;
    for (auto it = patch.begin(); it != patch.end(); ++it) p[it.key()] = it.value();
    return p;
}

void put(const std::string& key_suffix, const std::string& label, const std::string& hash, const Json& patch = Json::object(),
         double now = NOW) {
    upsert_proxy_profile({pack_of(pack_with(key_suffix, patch)), label, hash, now});
}

} // namespace

TEST_CASE("bundle: tokens, labels, order and rendering match Node (golden)") {
    const auto& g = w2h::fx()["bundle"];
    for (const auto& t : g["tokens"]) CHECK_EQ(estimate_tokens(t["t"].get<std::string>()), t["n"].get<double>());
    for (const auto& d : g["dims"]) CHECK_EQ(dim_label(d["d"].get<std::string>()), d["l"].get<std::string>());
    const auto pack = pack_of(g["pack"]);
    for (const auto& o : g["order"]) {
        Json ids = Json::array();
        for (const auto& e : order_evidence(pack.evidence, o["terms"].get<std::vector<std::string>>())) ids.push_back(e.id);
        CHECK_EQ(w2h::s(ids), w2h::s(o["ids"]));
    }
    for (const auto& r : g["renders"]) {
        const auto b = render_proxy_bundle(pack, {r["budgetTokens"].get<double>(), r["focusTerms"].get<std::vector<std::string>>(), "論者#aaaaaa"});
        CHECK_EQ(w2h::s(bundle_json(b)), w2h::s(r["bundle"]));
    }
    CHECK_EQ(w2h::s(bundle_json(render_proxy_bundle(pack_of(g["packNoStyle"]), {5000, {}, "論者#bbbbbb"}))), w2h::s(g["noStyle"]));
    auto low = pack;
    low.sufficiency.confidence = "low";
    CHECK(!render_proxy_bundle(low, {5000, {}, "x"}));
    auto revoked = pack;
    revoked.revoked = true;
    CHECK(!render_proxy_bundle(revoked, {5000, {}, "x"}));
}

TEST_CASE("bundle (proxy-debate.test.ts): no numbers, ids, AI notice, drops far evidence first") {
    const auto pack = pack_of(w2h::fx()["bundle"]["pack"]);
    const auto b = render_proxy_bundle(pack, {3500, {"周回"}, "論者#aaaaaa"});
    CHECK(b.has_value());
    CHECK(!str::contains(b->text, "0.6"));
    CHECK(str::contains(b->text, "[E1]") && str::contains(b->text, "難易度"));
    CHECK(str::contains(b->text, "本人ではなく"));
    const auto small = render_proxy_bundle(pack, {b->estimatedTokens - 40, {"周回"}, "論者#aaaaaa"});
    CHECK(small.has_value());
    CHECK(small->evidenceIds.size() < b->evidenceIds.size());
    CHECK(std::find(small->evidenceIds.begin(), small->evidenceIds.end(), "E1") != small->evidenceIds.end());
    CHECK(small->droppedEvidence > 0);
    CHECK(!render_proxy_bundle(pack, {50, {}, "x"}));
}

TEST_CASE("request / stance / sides: same as Node (golden)") {
    for (const auto& c : w2h::fx()["requests"]) CHECK_EQ(w2h::s(request_json(parse_proxy_request(c["text"].get<std::string>()))), w2h::s(c["request"]));
    for (const auto& c : w2h::fx()["stances"]) {
        const auto s = parse_proxy_stance(c["text"].get<std::string>());
        CHECK_EQ(w2h::s(s ? Json(*s) : Json(nullptr)), w2h::s(c["stance"]));
    }
    CHECK_EQ(build_proxy_stance_prompt("周回報酬は必要か", "束の本文"), w2h::fx()["stancePrompt"].get<std::string>());
    for (const auto& c : w2h::fx()["sides"])
        CHECK_EQ(w2h::s(to_json(assign_issue_sides(c["stances"].get<std::vector<std::string>>(), c["labels"].get<std::vector<std::string>>()))),
                 w2h::s(c["sides"]));
}

TEST_CASE("judge_proxy_stance: LLM failure / unknown label fall back to neutral") {
    std::vector<std::string> warns;
    persona_engine::MockLLMClient ok({[](const persona_engine::LLMInvokeArgs& a) {
        CHECK_EQ(a.maxTokens.value_or(0), 20);
        return std::variant<std::string, persona_engine::LLMResult>(std::string("答え: con"));
    }});
    CHECK_EQ(judge_proxy_stance({"論点", "束", &ok, std::string("m"), {}}), std::string("con"));
    CHECK_EQ(*ok.last_invocation()->model, std::string("m"));
    persona_engine::MockLLMClient bad({[](const persona_engine::LLMInvokeArgs&) {
        return std::variant<std::string, persona_engine::LLMResult>(persona_engine::LLMResult::failure("boom"));
    }});
    CHECK_EQ(judge_proxy_stance({"論点", "束", &bad, std::nullopt, [&](const std::string& m) { warns.push_back(m); }}), std::string("neutral"));
    persona_engine::MockLLMClient odd({[](const persona_engine::LLMInvokeArgs&) {
        return std::variant<std::string, persona_engine::LLMResult>(std::string("わからない"));
    }});
    CHECK_EQ(judge_proxy_stance({"論点", "束", &odd, std::nullopt, [&](const std::string& m) { warns.push_back(m); }}), std::string("neutral"));
    CHECK_EQ(warns.size(), 2u);
    CHECK(str::contains(warns[0], "boom"));
}

TEST_CASE("store: upsert keeps eval for same content, resets on change, revoke + castable") {
    w2h::TempFlowDb tmp;
    const std::string key = "ext:steam:00000000000000a1";
    CHECK_EQ(upsert_proxy_profile({pack_of(pack_with("a1")), "論者#aaaaaa", "h1", NOW}), std::string("inserted"));
    CHECK_EQ(upsert_proxy_profile({pack_of(pack_with("a1")), "論者#aaaaaa", "h1", NOW}), std::string("unchanged"));
    set_proxy_eval_status(key, "roguelike", "passed", "{}", NOW);
    upsert_proxy_profile({pack_of(pack_with("a1")), "論者#aaaaaa", "h1", NOW});
    CHECK_EQ(get_proxy_profile(key, "roguelike")->evalStatus, std::string("passed"));
    CHECK_EQ(upsert_proxy_profile({pack_of(pack_with("a1", Json{{"styleNotes", {"長めに書く"}}})), "論者#aaaaaa", "h2", NOW}),
             std::string("updated"));
    auto row = *get_proxy_profile(key, "roguelike");
    CHECK_EQ(row.evalStatus, std::string("unevaluated"));
    CHECK_EQ(row.pack.styleNotes, std::vector<std::string>{"長めに書く"});
    CHECK(!is_castable(row, NOW, false));
    CHECK_EQ(*is_castable(row, NOW, true), std::string("本人らしさの評価がまだ"));
    CHECK_EQ(*is_castable(row, row.expiresAt, false), std::string("有効期限が切れている"));
    CHECK(revoke_proxy_profile(key, "roguelike", NOW));
    CHECK(!revoke_proxy_profile(key, "roguelike", NOW));
    row = *get_proxy_profile(key, "roguelike");
    CHECK_EQ(*is_castable(row, NOW, false), std::string("失効している"));
    CHECK_EQ(upsert_proxy_profile({pack_of(pack_with("a1", Json{{"styleNotes", {"長めに書く"}}})), "論者#aaaaaa", "h2", NOW}),
             std::string("updated"));
    CHECK(!get_proxy_profile(key, "roguelike")->revoked);
    // DB の pack_json は TS と同じ JSON (契約の to_json)
    CHECK_EQ(db::get_flow_db()->get("SELECT pack_json FROM flow_proxy_profile")->str("pack_json"),
             json_stringify(to_json(pack_of(pack_with("a1", Json{{"styleNotes", {"長めに書く"}}})))));
    CHECK_EQ(list_proxy_profiles().size(), 1u);
    CHECK_EQ(find_proxy_profiles_by_label("論者#aaaaaa", std::string("roguelike")).size(), 1u);
    CHECK(find_proxy_profiles_by_label("論者#aaaaaa", std::string("other")).empty());
}

TEST_CASE("cast: parse + load participants with the same refusals (proxy-debate.test.ts)") {
    w2h::TempFlowDb tmp;
    put("a1", "論者#aaaaaa", "ha");
    put("b2", "論者#bbbbbb", "hb");
    const auto req = *parse_proxy_request("代理: 論者#aaaaaa, 論者#bbbbbb 周回報酬は必要か");
    CHECK_EQ(req.theme, std::string("周回報酬は必要か"));
    LoadProxyParticipantsArgs a;
    a.request = req;
    a.now = NOW;
    a.requirePassed = false;
    a.budgetTokens = 3500;
    a.models = {"m1", "m2"};
    const auto cast = load_proxy_participants(a);
    CHECK_EQ(cast.size(), 2u);
    for (const auto& p : cast) {
        CHECK_EQ(p.role, std::string("debater"));
        CHECK(p.proxy.has_value());
        CHECK_EQ(p.possession->label, std::string("AI代理"));
    }
    CHECK_EQ(cast[0].model, std::string("m1"));
    CHECK_EQ(cast[1].model, std::string("m2"));
    CHECK_EQ(cast[0].id, std::string("proxy:ext:steam:00000000000000a1:roguelike"));
    CHECK_EQ(cast[0].speechStyle, std::string("短く書く / 語尾が強い"));

    const auto expect_error = [&](LoadProxyParticipantsArgs args, const std::string& needle) {
        try {
            load_proxy_participants(args);
            CHECK(false);
        } catch (const ProxyRequestError& e) {
            CHECK(str::contains(e.what(), needle));
        }
    };
    auto b = a;
    b.requirePassed = true;
    expect_error(b, "評価がまだ");
    b = a;
    b.now = NOW + 60 * DAY;
    expect_error(b, "有効期限");
    b = a;
    b.request.labels = {"論者#aaaaaa"};
    expect_error(b, "2 人");
    b = a;
    b.request.labels = {"論者#aaaaaa", "論者#ffffff"};
    expect_error(b, "取り込まれていません");
    b = a;
    b.models = {"only"};
    CHECK_EQ(load_proxy_participants(b)[1].model, std::string("only"));
    b.models = {};
    expect_error(b, "モデルがありません");
    b = a;
    b.budgetTokens = 50;
    expect_error(b, "予算 50 トークン");
    b = a;
    b.request.genre = std::string("rpg");
    expect_error(b, "ジャンル rpg の人物像がありません");

    // 共通ジャンルが複数なら指定を求める
    put("a1", "論者#aaaaaa", "ha2", Json{{"genre", "rpg"}});
    put("b2", "論者#bbbbbb", "hb2", Json{{"genre", "rpg"}});
    expect_error(a, "共通するジャンルが複数あります (");
    b = a;
    b.request.genre = std::string("rpg");
    CHECK_EQ(load_proxy_participants(b)[0].proxy->genre, std::string("rpg"));
}

TEST_CASE("resolve_proxy_run: tag, enabled, confidential, missing directive, roster models") {
    w2h::TempFlowDb tmp;
    put("a1", "論者#aaaaaa", "ha");
    put("b2", "論者#bbbbbb", "hb");
    env::set_for_test("DISCUTERE_FLOW_PROXY_ENABLED", "true");
    env::set_for_test("DISCUTERE_FLOW_PROXY_REQUIRE_EVAL_PASSED", "false");
    env::set_for_test("DISCUTERE_FLOW_PROXY_BUDGET_TOKENS", "3500");
    env::set_for_test("DISCUTERE_FLOW_ROSTER_DISCUSSANTS", "claude-a@high,claude-b@high");
    config::reset_config_for_test();
    CHECK(!resolve_proxy_run("x", {}));
    CHECK_THROWS(resolve_proxy_run("代理: 論者#aaaaaa, 論者#bbbbbb x", {"代理", "機密"}));
    try {
        resolve_proxy_run("代理の指定なし", {"代理"});
        CHECK(false);
    } catch (const ProxyRequestError& e) {
        CHECK(str::contains(e.what(), "代理: 論者"));
    }
    const auto run = resolve_proxy_run("代理: 論者#aaaaaa, 論者#bbbbbb 周回報酬は必要か", {"代理"}, NOW);
    CHECK_EQ(run->theme, std::string("周回報酬は必要か"));
    CHECK_EQ(run->participants[0].model, std::string("claude-a@high"));
    CHECK_EQ(run->participants[1].model, std::string("claude-b@high"));
    env::set_for_test("DISCUTERE_FLOW_PROXY_ENABLED", "false");
    config::reset_config_for_test();
    try {
        resolve_proxy_run("代理: 論者#aaaaaa, 論者#bbbbbb x", {"代理"}, NOW);
        CHECK(false);
    } catch (const ProxyRequestError& e) {
        CHECK(str::contains(e.what(), "無効"));
    }
    for (const char* k : {"DISCUTERE_FLOW_PROXY_ENABLED", "DISCUTERE_FLOW_PROXY_REQUIRE_EVAL_PASSED", "DISCUTERE_FLOW_PROXY_BUDGET_TOKENS",
                          "DISCUTERE_FLOW_ROSTER_DISCUSSANTS"})
        env::set_for_test(k, std::nullopt);
    config::reset_config_for_test();
}

TEST_CASE("boundary: huge / adversarial theme texts are scanned linearly") {
    const auto t0 = std::chrono::steady_clock::now();
    std::string many_proxy;
    for (int i = 0; i < 200'000; ++i) many_proxy += "代理 :";
    CHECK(!parse_proxy_request(many_proxy));
    std::string labels = "代理: ";
    for (int i = 0; i < 100'000; ++i) labels += "論者#abcdef vs ";
    const auto r = parse_proxy_request(labels + "議題");
    CHECK_EQ(r->labels.size(), 1u);
    CHECK_EQ(r->theme, std::string("議題"));
    std::string words = std::string(1'000'000, 'p') + " pro";
    CHECK_EQ(*parse_proxy_stance(words), std::string("pro"));
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(10));
}

TEST_CASE("boundary: stored pack_json that is broken / too large / not in contract is refused") {
    w2h::TempFlowDb tmp;
    put("a1", "論者#aaaaaa", "ha");
    auto db = db::get_flow_db();
    db->run("UPDATE flow_proxy_profile SET pack_json = '{broken'");
    CHECK_THROWS(get_proxy_profile("ext:steam:00000000000000a1", "roguelike"));
    db->run("UPDATE flow_proxy_profile SET pack_json = ?", {"{\"pad\":\"" + std::string(kProxyPackJsonMaxBytes, 'x') + "\"}"});
    CHECK_THROWS(get_proxy_profile("ext:steam:00000000000000a1", "roguelike"));
    db->run("UPDATE flow_proxy_profile SET pack_json = '{\"speakerKey\":\"ext:steam:76561198000000000\"}'");
    CHECK_THROWS(find_proxy_profiles_by_label("論者#aaaaaa"));
}

DI_TEST_MAIN()
