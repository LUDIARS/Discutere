// w3-K: 経済グラフ (src/ludus/economy-analyzer.ts / src/api/economy-graph-routes.ts)。TS テストは無いので
// 純関数 (toSlug / extractJson)・画面の golden・LLM (claude -p 相当) に DB の情報を載せる分析・キャッシュ・ルートを確かめる。
#include <future>
#include <thread>

#include "check.hpp"
#include "core/test_util.hpp"

#include "api/economy_graph_routes.hpp"
#include "base/strings.hpp"
#include "core/index.hpp"
#include "db/repository.hpp"
#include "db/schema.hpp"
#include "ludus/economy_analyzer.hpp"
#include "persona_engine/llm/mock.hpp"

using namespace di;
using namespace di::ludus;
using persona_engine::LLMInvokeArgs;
using persona_engine::LLMResult;
using persona_engine::MockLLMClient;

namespace {

const std::string GRAPH_TEXT = R"(```json
{"nodes":[{"id":"gold","type":"resource","label":"Gold","description":"通貨"},"bad",{"id":"loop","type":"engine","label":"Loop","description":"周回"}],
 "edges":[{"id":"e1","source":"loop","target":"gold","type":"produces","label":"makes"}],
 "summary":"周回で金を稼ぐ","economyLoops":["周回→強化",3]}
```)";

struct World {
    test::TempDir dir;
    sql::DatabasePtr main;
    std::string kg, ev;
    World() {
        main = sql::Database::open(dir.file("discutere.db"));
        db::ensure_schema(*main);
        kg = dir.file("kg.sqlite");
        ev = dir.file("events.jsonl");
        auto core = core::create_core(kg, ev);
        core::repositories::SessionInput s;
        s.id = "s1";
        s.workspaceId = "knowledge";
        s.title = "周回の話";
        s.startedAt = 1;
        core->repos.session.create(s);
        core::repositories::UtteranceInput u;
        u.id = "u1";
        u.workspaceId = "knowledge";
        u.sessionId = "s1";
        u.rawContent = "Test Game の周回が楽しい";
        u.normalizedContent = "Test Game の周回が楽しい";
        u.postedAt = 1;
        core->repos.utterance.create(u);
        core->close();
        db::LudusMechanicRepo(main).create(Json{{"id", "m1"}, {"workspaceId", "knowledge"}, {"gameTitle", "Test Game"}, {"mechanic", "周回"},
                                                {"confidence", 80}, {"sourceUrl", "https://example.invalid/g"}, {"sourceTitle", "攻略"}});
    }
    AnalyzeEconomyDeps deps(std::shared_ptr<persona_engine::LLMClient> llm) {
        AnalyzeEconomyDeps d;
        d.llm = std::move(llm);
        d.mainDb = main;
        d.openCore = [this] { return core::create_core(kg, ev); };
        return d;
    }
};

} // namespace

TEST_CASE("to_slug / extract_json") {
    CHECK_EQ(to_slug("Hello World"), std::string("hello-world"));
    CHECK_EQ(to_slug("  --Vampire Survivors!!  "), std::string("vampire-survivors"));
    CHECK_EQ(to_slug("モンスターストライク"), std::string(""));
    CHECK_EQ(to_slug("\xC4\xB0stanbul"), std::string("i-stanbul"));
    CHECK_EQ(extract_json("```json\n{\"a\":1}\n```"), std::string("{\"a\":1}"));
    CHECK_EQ(extract_json("前 ``` {\"a\":2} ``` 後"), std::string("{\"a\":2}"));
    CHECK_EQ(extract_json("x {\"a\":3} y"), std::string("{\"a\":3}"));
    CHECK_EQ(extract_json("} {"), std::string(""));
    CHECK_EQ(extract_json("  plain  "), std::string("plain"));
    CHECK_EQ(extract_json("```json```"), std::string(""));
    CHECK_EQ(extract_json("``` only one fence {\"b\":1}"), std::string("{\"b\":1}"));
}

TEST_CASE("golden: 経済グラフの画面 (Node の出力と 1 バイト単位で一致)・<script> を閉じる値は無害化") {
    for (const auto& c : test::load_fixture("w3k/economy_graph_html.json")) {
        auto router = api::create_economy_graph_routes(api::EconomyGraphRoutesDeps{});
        http::Request req;
        req.method = "GET";
        req.target = req.path = "/ludus/economy-graph/" + c["slug"].get<std::string>();
        if (c.contains("host")) req.headers.set("host", c["host"].get<std::string>());
        if (c.contains("proto")) req.headers.set("x-forwarded-proto", c["proto"].get<std::string>());
        const auto res = router.handle(req);
        CHECK_EQ(res.status, c["status"].get<int>());
        CHECK_EQ(res.body, c["html"].get<std::string>());
    }
    const std::string html = api::build_economy_graph_html("</script><script>alert(1)</script>", "http://h \"&<x>");
    CHECK(!str::contains(html, "</script><script>alert"));
    CHECK(str::contains(html, R"(const SLUG = "</script><script>alert(1)</script>";)"));
    CHECK(str::contains(html, R"(const API_BASE = "http://h \"&<x>";)"));
    // 置き換えた値の中の {{...}} は再走査しない
    CHECK(str::contains(api::build_economy_graph_html("{{apiBaseJson}}", "b"), R"(const SLUG = "{{apiBaseJson}}";)"));
}

TEST_CASE("analyze_economy: DB の情報をプロンプトに載せて claude -p 相当で 1 回呼ぶ・graph を整えて保存・キャッシュ") {
    World w;
    std::optional<LLMInvokeArgs> seen;
    auto llm = std::make_shared<MockLLMClient>(std::vector<persona_engine::MockResponder>{[&](const LLMInvokeArgs& a) {
        seen = a;
        return std::variant<std::string, LLMResult>(GRAPH_TEXT);
    }});
    const auto r = analyze_economy("knowledge", "Test Game", w.deps(llm));
    CHECK(r.ok && !r.cached);
    CHECK_EQ(llm->calls(), 1);
    CHECK(seen && seen->model == std::string(ANALYSIS_MODEL) && seen->maxTokens == ANALYSIS_MAX_TOKENS && seen->timeoutMs == ANALYSIS_TIMEOUT_MS);
    CHECK(seen->conversationOnly);
    CHECK_EQ(seen->system.value_or(""), economy_system_prompt());
    CHECK(str::contains(seen->prompt, "Analyze the internal economy and resource relationships of: **Test Game**"));
    CHECK(str::contains(seen->prompt, "- 周回 (confidence: 80%, source: 攻略)"));
    CHECK(str::contains(seen->prompt, "Discussion knowledge for \"Test Game\" (1 results):\n[周回の話] Test Game の周回が楽しい"));
    CHECK(str::contains(seen->prompt, "Discussion knowledge for \"周回\" (1 results):"));
    CHECK_EQ(r.graph.nodes.size(), std::size_t{2});  // object 以外は捨てる
    CHECK_EQ(r.graph.edges.size(), std::size_t{1});
    CHECK((r.graph.economyLoops == std::vector<std::string>{"周回→強化"}));
    CHECK_EQ(r.graph.mechanicCount, 1.0);
    const auto saved = db::EconomyGraphRepo(w.main).find_by_game("knowledge", "Test Game");
    CHECK(saved.has_value());
    const Json g = parse_json_or_throw(saved->graphJson);
    CHECK_EQ(json_stringify(g), json_stringify(to_json(r.graph)));
    // キーの順は TS と同じ
    std::vector<std::string> keys;
    for (auto it = g.begin(); it != g.end(); ++it) keys.push_back(it.key());
    CHECK((keys == std::vector<std::string>{"gameTitle", "nodes", "edges", "summary", "economyLoops", "analyzedAt", "mechanicCount"}));

    // 2 回目はキャッシュ (LLM を呼ばない)、force で再分析
    const auto cached = analyze_economy("knowledge", "Test Game", w.deps(llm));
    CHECK(cached.ok && cached.cached && llm->calls() == 1);
    CHECK_EQ(json_stringify(to_json(cached.graph)), json_stringify(g));
    analyze_economy("knowledge", "Test Game", w.deps(llm), true);
    CHECK_EQ(llm->calls(), 2);
    CHECK_EQ(db::EconomyGraphRepo(w.main).list_by_workspace("knowledge").size(), std::size_t{1});
}

TEST_CASE("analyze_economy: LLM 未設定・失敗・空応答・壊れた JSON・巨大応答・KG が開けない") {
    World w;
    const auto no_llm = analyze_economy("knowledge", "Other", w.deps(nullptr));
    CHECK(!no_llm.ok && no_llm.error == "LLM not configured");
    auto fail = std::make_shared<MockLLMClient>(std::vector<persona_engine::MockResponder>{
        [](const LLMInvokeArgs&) { return std::variant<std::string, LLMResult>(LLMResult::failure("claude -p timeout")); }});
    CHECK_EQ(analyze_economy("knowledge", "Other", w.deps(fail)).error, std::string("claude -p timeout"));
    auto empty = std::make_shared<MockLLMClient>(std::vector<persona_engine::MockResponder>{
        [](const LLMInvokeArgs&) { return std::variant<std::string, LLMResult>(std::string("   ")); }});
    CHECK_EQ(analyze_economy("knowledge", "Other", w.deps(empty)).error, std::string("empty response"));
    auto broken = std::make_shared<MockLLMClient>(std::vector<persona_engine::MockResponder>{
        [](const LLMInvokeArgs&) { return std::variant<std::string, LLMResult>(std::string("{ not json }")); }});
    const auto br = analyze_economy("knowledge", "Other", w.deps(broken));
    CHECK(!br.ok && str::starts_with(br.error, "JSON parse failed: "));
    auto huge = std::make_shared<MockLLMClient>(std::vector<persona_engine::MockResponder>{
        [](const LLMInvokeArgs&) { return std::variant<std::string, LLMResult>("{\"nodes\":[],\"pad\":\"" + std::string(kEconomyMaxResponseBytes, 'a') + "\"}"); }});
    CHECK(!analyze_economy("knowledge", "Other", w.deps(huge)).ok);
    auto deep = std::make_shared<MockLLMClient>(std::vector<persona_engine::MockResponder>{
        [](const LLMInvokeArgs&) { return std::variant<std::string, LLMResult>("{\"nodes\":" + std::string(5000, '[') + std::string(5000, ']') + "}"); }});
    CHECK(!analyze_economy("knowledge", "Other", w.deps(deep)).ok);
    CHECK(!db::EconomyGraphRepo(w.main).find_by_game("knowledge", "Other"));
    // KG が開けなくても分析は続ける (議論知識は「利用不可」の定型文)
    std::string prompt;
    auto ok = std::make_shared<MockLLMClient>(std::vector<persona_engine::MockResponder>{[&](const LLMInvokeArgs& a) {
        prompt = a.prompt;
        return std::variant<std::string, LLMResult>(std::string(R"({"nodes":[],"edges":[]})"));
    }});
    auto d = w.deps(ok);
    d.openCore = []() -> std::unique_ptr<core::Core> { throw std::runtime_error("no kg"); };
    const auto r = analyze_economy("knowledge", "Other", d);
    CHECK(r.ok && r.graph.summary.empty() && r.graph.economyLoops.empty());
    CHECK(str::contains(prompt, "Discussion knowledge search unavailable (KG not initialized or empty)."));
    CHECK(str::contains(prompt, "No mechanics found in guide data for \"Other\"."));
    // キャッシュ (DB の値) が壊れていたら失敗を返す
    db::EconomyGraphRepo(w.main).upsert(Json{{"id", "x"}, {"workspaceId", "knowledge"}, {"gameTitle", "Broken"}, {"graphJson", "{oops"}, {"mechanicCount", 0}});
    CHECK(!analyze_economy("knowledge", "Broken", w.deps(ok)).ok);
}

TEST_CASE("routes: analyze-economy / economy-graphs / economy-graph/:slug") {
    World w;
    std::vector<std::pair<std::string, bool>> started;
    api::EconomyGraphRoutesDeps deps;
    deps.workspaceId = "knowledge";
    deps.analyze.mainDb = w.main;
    deps.startAnalysis = [&](const std::string&, const std::string& title, bool force) {
        started.emplace_back(title, force);
        return true;
    };
    auto router = api::create_economy_graph_routes(deps);
    auto call = [&](const std::string& method, const std::string& path, const std::string& body = "") {
        http::Request req;
        req.method = method;
        req.target = req.path = path;
        req.body = body;
        return router.handle(req);
    };
    CHECK_EQ(call("POST", "/api/ludus/analyze-economy", "{}").status, 400);
    CHECK_EQ(call("POST", "/api/ludus/analyze-economy", "not json").status, 400);
    CHECK_EQ(call("POST", "/api/ludus/analyze-economy", R"({"gameTitle":3})").status, 400);
    CHECK_EQ(call("POST", "/api/ludus/analyze-economy", R"({"slug":"unknown"})").status, 400);
    const auto ok = call("POST", "/api/ludus/analyze-economy", R"({"gameTitle":"Test Game","force":1})");
    CHECK_EQ(ok.status, 200);
    CHECK_EQ(ok.body, std::string(R"({"ok":true,"slug":"test-game","message":"analysis started"})"));
    CHECK(started.size() == 1 && started[0].first == "Test Game" && started[0].second);
    // gameTitle の上限 (500 / 501)
    CHECK_EQ(call("POST", "/api/ludus/analyze-economy", json_stringify(Json{{"gameTitle", std::string(500, 'a')}})).status, 200);
    CHECK_EQ(call("POST", "/api/ludus/analyze-economy", json_stringify(Json{{"gameTitle", std::string(501, 'a')}})).status, 400);

    CHECK_EQ(call("GET", "/api/ludus/economy-graph/test-game").status, 404);
    db::EconomyGraphRepo(w.main).upsert(
        Json{{"id", "g1"}, {"workspaceId", "knowledge"}, {"gameTitle", "Test Game"}, {"graphJson", R"({"gameTitle":"Test Game","nodes":[]})"}, {"mechanicCount", 2}});
    const auto list = call("GET", "/api/ludus/economy-graphs");
    const Json lj = parse_json_or_throw(list.body);
    CHECK(lj.size() == 1 && lj[0]["slug"] == "test-game" && lj[0]["mechanicCount"] == 2);
    CHECK(str::ends_with(lj[0]["updatedAt"].get<std::string>(), ".000Z"));
    CHECK_EQ(call("GET", "/api/ludus/economy-graph/test-game").body, std::string(R"({"gameTitle":"Test Game","nodes":[]})"));
    // slug から既存 graph の gameTitle を引いて再分析
    CHECK_EQ(call("POST", "/api/ludus/analyze-economy", R"({"slug":"test-game"})").status, 200);
    CHECK_EQ(started.back().first, std::string("Test Game"));
    CHECK(!started.back().second);
    // 本文上限
    CHECK_EQ(router.max_body_for("POST", "/api/ludus/analyze-economy"), api::kEconomyAnalyzeMaxBody);
    // キャッシュの値が壊れていれば 500
    db::EconomyGraphRepo(w.main).upsert(Json{{"id", "g2"}, {"workspaceId", "knowledge"}, {"gameTitle", "Bad"}, {"graphJson", "{"}, {"mechanicCount", 0}});
    CHECK_EQ(call("GET", "/api/ludus/economy-graph/bad").status, 500);
}

TEST_CASE("analyze_economy_in_background: 同じゲームの分析は重ねない") {
    World w;
    std::promise<void> gate;
    auto gate_future = gate.get_future().share();
    std::atomic<int> calls{0};
    auto llm = std::make_shared<MockLLMClient>(std::vector<persona_engine::MockResponder>{[&](const LLMInvokeArgs&) {
        ++calls;
        gate_future.wait();
        return std::variant<std::string, LLMResult>(std::string(R"({"nodes":[]})"));
    }});
    CHECK(analyze_economy_in_background("knowledge", "Busy", w.deps(llm)));
    CHECK(!analyze_economy_in_background("knowledge", "Busy", w.deps(llm)));
    gate.set_value();
    for (int i = 0; i < 200 && !db::EconomyGraphRepo(w.main).find_by_game("knowledge", "Busy"); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(db::EconomyGraphRepo(w.main).find_by_game("knowledge", "Busy").has_value());
    CHECK_EQ(calls.load(), 1);
    // 背景の仕事が後始末 (実行中の印を外す) を終えるのを待ってから World を壊す
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
}

DI_TEST_MAIN()
