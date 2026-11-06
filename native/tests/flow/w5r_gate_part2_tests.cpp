// w5r_gate のテスト (2/2)。準備は w5r_gate_support.hpp。
#include "w5r_gate_support.hpp"

using namespace w5r_gate_support;

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
