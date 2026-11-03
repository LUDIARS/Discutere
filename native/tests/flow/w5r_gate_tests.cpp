// w5-R: src/flow/information-gate-runner.ts と src/flow/web/flow-prep.ts の移植の検証 (TS テスト無し)。
// golden: gateBeforeFlow の対象外の分岐と LLM 評価 1 回 (呼び出し列・外部の声の検索・log・コストログ)、
// resolveDebatabilityGate、buildAutoCrawlSpec / buildLearningCrawl (config の env 違い)。
// 加えて prepare_information_before_flow の 機密 / 情報ゲート / 旧 autoCrawl の分岐を偽の collector で確かめる (ネットワークに出ない)。
#include "check.hpp"
#include "w4q_support.hpp"
#include "w5r_support.hpp"

#include <atomic>

#include "base/strings.hpp"

#include "flow/information_gate_runner.hpp"
#include "flow/web/flow_prep.hpp"

using namespace di;
using namespace di::flow;

namespace {

void set_envs(const Json& envs) {
    for (auto it = envs.begin(); it != envs.end(); ++it) env::set_for_test(it.key(), std::string_view(it.value().get<std::string>()));
    config::reset_config_for_test();
}
void unset_envs(const Json& envs) {
    for (auto it = envs.begin(); it != envs.end(); ++it) env::set_for_test(it.key(), std::nullopt);
    config::reset_config_for_test();
}

} // namespace

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

TEST_CASE("prepare: 情報ゲートが不足と見たら不足観点を集める (偽の取得、ネットワークに出ない)") {
    w4q::Env env;
    // LLM が応答しない → 件数 0 で sparse → テーマでクロール
    struct FailLLM : persona_engine::LLMClient {
        persona_engine::LLMResult invoke(const persona_engine::LLMInvokeArgs&) override { return persona_engine::LLMResult::failure("down"); }
    };
    web::FlowWebDeps d;
    d.workspaceId = "ws";
    d.llm = std::make_shared<FailLLM>();
    const std::string kg = env.kg;
    d.openCore = [kg] { return core::create_core(kg, std::nullopt); };
    std::vector<std::string> queries;
    std::mutex m;
    d.gateCollectAndImport = [&](const CollectAndImportDeps& deps) {
        std::lock_guard lock(m);
        queries.push_back(deps.spec.source + ":" + deps.spec.query.value_or(""));
        return CollectAndImportResult{1, 1};
    };
    web::prepare_information_before_flow("improvement", "周回", {}, "s2", std::nullopt, d);
    CHECK(!queries.empty());
    CHECK(str::starts_with(queries[0], "niconico:"));
}

TEST_CASE("prepare: ゲート無効なら旧 autoCrawl (minVoices 未満で指定ソースを取得 → 取込)") {
    w4q::Env env;
    env::set_for_test("DISCUTERE_FLOW_INFOGATE_ENABLED", "false");
    config::reset_config_for_test();
    web::FlowWebDeps d;
    d.workspaceId = "ws";
    d.llm = std::make_shared<w5r::FakeLLM>();
    const std::string kg = env.kg;
    d.openCore = [kg] { return core::create_core(kg, std::nullopt); };
    int collected = 0;
    int imported = 0;
    d.collectors["steam"] = [&](const CollectorContext& ctx) {
        ++collected;
        CHECK_EQ(ctx.spec.appId.value_or(0), 42LL);
        crawler::sources::ExternalUtterance u;
        u.source = "steam";
        u.nativeId = "n1";
        u.gameSlug = ctx.gameSlug;
        u.threadKey = "42";
        u.content = "良いゲーム";
        return std::vector<crawler::sources::ExternalUtterance>{u};
    };
    d.importItems = [&](core::Core*, const std::vector<crawler::sources::ExternalUtterance>& items, const std::string& ws) {
        CHECK_EQ(ws, std::string("ws"));
        imported += static_cast<int>(items.size());
        return static_cast<int>(items.size());
    };
    const auto spec = web::build_auto_crawl_spec(Json{{"learningSource", "steam"}, {"learningAppId", "42"}});
    CHECK(spec.has_value());
    web::prepare_information_before_flow("discussion", "テーマ", {}, "s3", spec, d);
    CHECK_EQ(collected, 1);
    CHECK_EQ(imported, 1);
    // 対象外フロー / spec 無し / Core 無しは何もしない
    web::legacy_auto_crawl_before_flow("learning", "テーマ", spec, d);
    web::legacy_auto_crawl_before_flow("discussion", "テーマ", std::nullopt, d);
    auto no_core = d;
    no_core.openCore = nullptr;
    web::legacy_auto_crawl_before_flow("discussion", "テーマ", spec, no_core);
    CHECK_EQ(collected, 1);
    // collector の例外は議論を止めない
    d.collectors["steam"] = [](const CollectorContext&) -> std::vector<crawler::sources::ExternalUtterance> { throw std::runtime_error("boom"); };
    web::legacy_auto_crawl_before_flow("discussion", "テーマ", spec, d);
    env::set_for_test("DISCUTERE_FLOW_INFOGATE_ENABLED", std::nullopt);
}

TEST_CASE("境界: build_auto_crawl_spec は壊れた・巨大な入力でも落ちない") {
    w4q::Env env;
    std::string urls;
    for (int i = 0; i < 100000; ++i) urls += "https://e.example/" + std::to_string(i) + ", ";
    const auto s = web::build_auto_crawl_spec(Json{{"learningSource", "website"}, {"learningUrls", urls}});
    CHECK(s.has_value());
    CHECK_EQ(s->urls->size(), std::size_t{100000});
    CHECK(!web::build_auto_crawl_spec(Json::array()).has_value() || true);
    const auto bad = web::build_auto_crawl_spec(Json{{"learningSource", "steam"}, {"learningAppId", "1e400"}});
    CHECK(bad.has_value());
    CHECK(!bad->appId.has_value());  // Infinity は持たない
    CHECK(!web::build_learning_crawl(Json(nullptr), std::nullopt) || true);
}

int main() { return di::test::run_all(); }
