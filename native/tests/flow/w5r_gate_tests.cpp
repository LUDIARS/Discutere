// w5r_gate のテスト (1/2)。準備は w5r_gate_support.hpp。
#include "w5r_gate_support.hpp"

using namespace w5r_gate_support;

TEST_CASE("golden: buildAutoCrawlSpec / buildLearningCrawl / webPaperGateEnabled が Node と一致") {
    w4q::Env env;
    const Json fx = w5r::load_fixture("flow_prep.json");
    for (const auto& block : fx) {
        set_envs(block["env"]);
        CHECK_EQ(web::web_paper_gate_enabled(), block["webPaperGateEnabled"].get<bool>());
        for (const auto& c : block["cases"]) {
            const auto what = block["env"].dump() + " " + c["body"].dump();
            const auto a = web::build_auto_crawl_spec(c["body"]);
            W5R_CHECK_JSON(a ? web::to_json(*a) : Json(nullptr), c["autoCrawl"], "autoCrawl " + what);
            const auto l = web::build_learning_crawl(c["body"], std::nullopt);
            W5R_CHECK_JSON(l ? web::to_json(*l) : Json(nullptr), c["learning"], "learning " + what);
            const auto k = web::build_learning_crawl(c["body"], std::string("fake-key"));
            W5R_CHECK_JSON(k ? web::to_json(*k) : Json(nullptr), c["learningWithKey"], "learningWithKey " + what);
        }
        unset_envs(block["env"]);
    }
}

TEST_CASE("golden: gateBeforeFlow / resolveDebatabilityGate が Node と一致") {
    w4q::Env env;
    const Json fx = w5r::load_fixture("gate.json");
    w5r::FakeLLM llm;
    w5r::FakeVoices voices;
    const std::string kg = env.kg;
    const std::string events = env.dir.file("events.jsonl");
    for (const auto& c : fx["cases"]) {
        set_envs(c["env"]);
        llm.take_calls();
        voices.take();
        Json logs = Json::array();
        FlowGateArgs a;
        a.kind = c["kind"].get<std::string>();
        a.theme = "周回ゲームの報酬";
        a.tags = c["tags"].get<std::vector<std::string>>();
        a.llm = &llm;
        if (c["withCore"].get<bool>()) a.openCore = [kg, events] { return core::create_core(kg, events); };
        a.workspaceId = "ws";
        a.listExternalVoices = voices.fn();
        a.sessionId = "gate-sess";
        a.log = [&](const std::string& m) { logs.push_back("log:" + m); };
        a.warn = [&](const std::string& m) { logs.push_back("warn:" + m); };
        // 万一クロールに進んでもネットワークに出ない
        a.collectAndImportFn = [](const CollectAndImportDeps&) -> CollectAndImportResult { throw std::runtime_error("network not allowed in test"); };
        const auto res = gate_before_flow(a);
        const auto label = c["label"].get<std::string>();
        W5R_CHECK_JSON(res ? to_json(*res) : Json(nullptr), c["result"], label + " result");
        W5R_CHECK_JSON(llm.take_calls(), c["calls"], label + " calls");
        W5R_CHECK_JSON(voices.take(), c["voiceCalls"], label + " voiceCalls");
        W5R_CHECK_JSON(logs, c["logs"], label + " logs");
        unset_envs(c["env"]);
    }
    Json rows = Json::array();
    for (const auto& r : db::get_flow_db()->all("SELECT flow, session_id, location FROM llm_call_log ORDER BY id")) rows.push_back(r.to_json());
    W5R_CHECK_JSON(rows, fx["costRows"], "costRows");

    for (const auto& c : fx["debatabilityGate"]) {
        set_envs(c["env"]);
        const auto g = resolve_debatability_gate(c["kind"].get<std::string>(), "s", llm);
        W5R_CHECK_JSON((g ? Json{{"minArmableIssues", g->minArmableIssues}} : Json(nullptr)), c["out"], c.dump());
        if (g) {
            // 包んだ LLM はコストログ付き (location=debatability-gate)
            CHECK(g->llm != nullptr);
            CHECK(g->to_paper_gate().llm == g->llm.get());
        }
        unset_envs(c["env"]);
    }
}

TEST_CASE("prepare: 機密/内部は外部収集をしない (LLM・collector を一度も呼ばない)") {
    w4q::Env env;
    auto llm = std::make_shared<w5r::FakeLLM>();
    std::atomic<int> collected{0}, opened{0};
    web::FlowWebDeps d;
    d.workspaceId = "ws";
    d.llm = llm;
    const std::string kg = env.kg;
    d.openCore = [&, kg] {
        ++opened;
        return core::create_core(kg, std::nullopt);
    };
    d.collectors["niconico"] = [&](const CollectorContext&) {
        ++collected;
        return std::vector<crawler::sources::ExternalUtterance>{};
    };
    d.gateCollectAndImport = [&](const CollectAndImportDeps&) {
        ++collected;
        return CollectAndImportResult{};
    };
    AutoCrawlSpec spec;
    spec.source = "niconico";
    for (const char* tag : {"機密", "内部"}) {
        web::prepare_information_before_flow("discussion", "テーマ", {tag}, "s1", spec, d);
    }
    CHECK_EQ(llm->take_calls().size(), std::size_t{0});
    CHECK_EQ(collected.load(), 0);
    CHECK_EQ(opened.load(), 0);
}

int main() { return di::test::run_all(); }
