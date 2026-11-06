// w3k_gate のテスト (2/4)。準備は w3k_gate_support.hpp。
#include "w3k_gate_support.hpp"

using namespace w3k_gate_support;

TEST_CASE("evaluate: 正常 JSON / LLM 失敗 → 件数 fallback / 非 JSON → fallback / gaps 正規化") {
    {
        ScriptLlm llm({eval_json("rich")});
        EvaluateArgs a{"t", {}, {}, &llm, std::nullopt, {}};
        const auto ev = evaluate_information_density(a);
        CHECK_EQ(ev.density, std::string("rich"));
        CHECK(ev.gaps.empty());
    }
    {
        ScriptLlm llm({}, true);
        std::vector<std::string> warns;
        EvaluateArgs a{"t", {}, std::vector<ContextVoice>(4, ContextVoice{"c", "s", std::nullopt}), &llm, std::nullopt,
                       [&](const std::string& m) { warns.push_back(m); }};
        CHECK_EQ(evaluate_information_density(a).density, std::string("moderate"));
        CHECK(warns.size() == 1 && str::contains(warns[0], "down"));
    }
    {
        ScriptLlm llm({"JSON ではない文章"});
        EvaluateArgs a{"t", {}, {}, &llm, std::nullopt, {}};
        const auto ev = evaluate_information_density(a);
        CHECK_EQ(ev.density, std::string("sparse"));
        CHECK(ev.gaps.empty());
    }
    {
        ScriptLlm llm({eval_json("sparse", Json::array({Json{{"aspect", "否定的意見"}, {"reason", "偏り"}, {"query", ""}, {"source", "reddit"}},
                                                         Json{{"aspect", ""}}}))});
        EvaluateArgs a{"周回設計", {}, {}, &llm, std::nullopt, {}};
        const auto ev = evaluate_information_density(a);
        CHECK_EQ(ev.gaps.size(), std::size_t{1});
        CHECK_EQ(ev.gaps[0].aspect, std::string("否定的意見"));
        CHECK_EQ(ev.gaps[0].query, std::string("周回設計 否定的意見"));
        CHECK_EQ(ev.gaps[0].source.value_or(""), std::string("reddit"));
    }
    {
        // LLM 未設定・例外も fallback
        EvaluateArgs a{"t", {}, std::vector<ContextVoice>(9, ContextVoice{"c", "s", std::nullopt}), nullptr, std::nullopt, {}};
        CHECK_EQ(evaluate_information_density(a).density, std::string("rich"));
        struct Throwing : persona_engine::LLMClient {
            persona_engine::LLMResult invoke(const persona_engine::LLMInvokeArgs&) override { throw std::runtime_error("boom"); }
        } thrower;
        a.llm = &thrower;
        CHECK_EQ(evaluate_information_density(a).density, std::string("rich"));
    }
}

TEST_CASE("evaluate: LLM 応答の上限 (ちょうど・+1・巨大・深い入れ子・件数上限)") {
    // extract_json_object は 1MiB まで。ちょうど 1MiB の JSON は読める、+1 は fallback。
    const std::string head = "{\"density\":\"rich\",\"pad\":\"";
    const std::string tail = "\"}";
    const std::size_t limit = 1u << 20;
    std::string exact = head + std::string(limit - head.size() - tail.size(), 'a') + tail;
    CHECK_EQ(exact.size(), limit);
    {
        ScriptLlm llm({exact});
        EvaluateArgs a{"t", {}, {}, &llm, std::nullopt, {}};
        CHECK_EQ(evaluate_information_density(a).density, std::string("rich"));
    }
    {
        ScriptLlm llm({head + std::string(limit - head.size() - tail.size() + 1, 'a') + tail});
        EvaluateArgs a{"t", {}, {}, &llm, std::nullopt, {}};
        CHECK_EQ(evaluate_information_density(a).density, std::string("sparse"));
    }
    {
        ScriptLlm llm({std::string(32u << 20, '{')});
        EvaluateArgs a{"t", {}, {}, &llm, std::nullopt, {}};
        CHECK_EQ(evaluate_information_density(a).density, std::string("sparse"));
    }
    {
        std::string deep = "{\"density\":\"rich\",\"x\":" + std::string(5000, '[') + std::string(5000, ']') + "}";
        ScriptLlm llm({deep});
        EvaluateArgs a{"t", {}, {}, &llm, std::nullopt, {}};
        CHECK_EQ(evaluate_information_density(a).density, std::string("sparse"));
    }
    {
        Json gaps = Json::array();
        Json covered = Json::array();
        for (int i = 0; i < 500; ++i) {
            gaps.push_back(Json{{"aspect", "a" + std::to_string(i)}});
            covered.push_back("c");
        }
        ScriptLlm llm({json_stringify(Json{{"density", "sparse"}, {"covered", covered}, {"gaps", gaps}})});
        EvaluateArgs a{"t", {}, {}, &llm, std::nullopt, {}};
        const auto ev = evaluate_information_density(a);
        CHECK_EQ(ev.gaps.size(), kMaxGapItems);
        CHECK_EQ(ev.covered.size(), kMaxCoveredItems);
    }
}

TEST_CASE("gate: sparse → 1 回学習 → rich で十分") {
    Growing voices;
    FakeCollect fc{&voices};
    ScriptLlm llm({eval_json("sparse", Json::array({Json{{"aspect", "否定"}, {"reason", "r"}, {"query", "t 否定"}, {"source", "niconico"}}})),
                   eval_json("rich")});
    InformationGateDeps d;
    d.theme = "t";
    d.workspaceId = "knowledge";
    d.llm = &llm;
    d.listExternalVoices = voices.list();
    d.crawlSource = "niconico";
    d.config = base_config();
    d.collectAndImportFn = fc.fn();
    const auto r = run_information_gate(d);
    CHECK(r.ran && r.sufficient);
    CHECK_EQ(r.initialDensity, std::string("sparse"));
    CHECK_EQ(r.finalDensity, std::string("rich"));
    CHECK_EQ(r.crawls, 1);
    CHECK_EQ(r.importedTotal, 5);
    CHECK(fc.calls.size() == 1 && fc.calls[0].second == "t 否定");
    CHECK((r.crawledQueries == std::vector<std::string>{"niconico:t 否定"}));
    CHECK(str::starts_with(r.message, "情報充分 (密度 sparse→rich ≥ moderate / 学習 1 回・取込 5 件)"));
}

TEST_CASE("gate: 常に sparse → 反復上限 / 初回 moderate → 学習しない / LLM 全失敗でも材料が厚ければ通す") {
    {
        Growing voices;
        FakeCollect fc{&voices, 0};
        ScriptLlm llm({eval_json("sparse", Json::array({Json{{"aspect", "a"}, {"reason", "r"}, {"query", "q1"}}}))});
        InformationGateDeps d;
        d.theme = "t";
        d.llm = &llm;
        d.listExternalVoices = voices.list();
        d.crawlSource = "niconico";
        d.config = base_config(2, 1);
        d.collectAndImportFn = fc.fn();
        const auto r = run_information_gate(d);
        CHECK_EQ(r.crawls, 2);
        CHECK(!r.sufficient);
        CHECK_EQ(fc.calls.size(), std::size_t{2});
        CHECK(str::starts_with(r.message, "目標 (moderate) に未達"));
    }
    {
        Growing voices;
        FakeCollect fc{&voices};
        ScriptLlm llm({eval_json("moderate")});
        InformationGateDeps d;
        d.theme = "t";
        d.llm = &llm;
        d.listExternalVoices = voices.list();
        d.crawlSource = "niconico";
        d.config = base_config();
        d.collectAndImportFn = fc.fn();
        const auto r = run_information_gate(d);
        CHECK(r.crawls == 0 && r.sufficient && fc.calls.empty());
    }
    {
        Growing voices;
        voices.n = 10;
        FakeCollect fc{&voices};
        ScriptLlm llm({}, true);
        InformationGateDeps d;
        d.theme = "t";
        d.llm = &llm;
        d.listExternalVoices = voices.list();
        d.crawlSource = "niconico";
        d.config = base_config();
        d.collectAndImportFn = fc.fn();
        const auto r = run_information_gate(d);
        CHECK_EQ(r.finalDensity, std::string("rich"));
        CHECK(r.sufficient && r.crawls == 0 && fc.calls.empty());
    }
}

TEST_CASE("gate: gaps 空ならテーマで学習・複数ソース横断・crawlSource へのフォールバック・失敗の継続") {
    {
        Growing voices;
        FakeCollect fc{&voices, 0};
        ScriptLlm llm({eval_json("sparse")});
        InformationGateDeps d;
        d.theme = "テーマX";
        d.llm = &llm;
        d.listExternalVoices = voices.list();
        d.crawlSource = "niconico";
        d.config = base_config(1, 2);
        d.collectAndImportFn = fc.fn();
        const auto r = run_information_gate(d);
        CHECK_EQ(r.crawls, 1);
        CHECK(fc.calls.size() == 1 && fc.calls[0].second == "テーマX");
    }
    {
        Growing voices;
        FakeCollect fc{&voices};
        ScriptLlm llm({eval_json("sparse", Json::array({Json{{"aspect", "否定"}, {"reason", "r"}, {"query", "t 否定"}}})), eval_json("rich")});
        InformationGateDeps d;
        d.theme = "t";
        d.llm = &llm;
        d.listExternalVoices = voices.list();
        d.crawlSources = {"niconico", "youtube"};
        d.crawlSource = "steam";  // crawlSources が優先
        d.config = base_config();
        d.collectAndImportFn = fc.fn();
        const auto r = run_information_gate(d);
        CHECK_EQ(r.crawls, 1);
        CHECK_EQ(fc.calls.size(), std::size_t{2});
        CHECK(fc.calls[0].first == "niconico" && fc.calls[1].first == "youtube");
        CHECK(fc.calls[0].second == "t 否定" && fc.calls[1].second == "t 否定");
        CHECK_EQ(r.importedTotal, 10);
    }
    {
        Growing voices;
        std::vector<std::string> warns;
        ScriptLlm llm({eval_json("sparse", Json::array({Json{{"aspect", "a"}, {"query", "q"}}})), eval_json("rich")});
        InformationGateDeps d;
        d.theme = "t";
        d.llm = &llm;
        d.listExternalVoices = [](const std::vector<std::string>&, int) -> std::vector<ContextVoice> { throw std::runtime_error("db"); };
        d.crawlSources = {"niconico", "youtube"};
        d.config = base_config();
        d.warn = [&](const std::string& m) { warns.push_back(m); };
        d.collectAndImportFn = [](const CollectAndImportDeps& c) -> CollectAndImportResult {
            if (c.spec.source == "youtube") throw std::runtime_error("yt boom");
            return {3, 3};
        };
        const auto r = run_information_gate(d);
        CHECK_EQ(r.importedTotal, 3);
        CHECK((r.crawledQueries == std::vector<std::string>{"niconico:q"}));
        CHECK(warns.size() == 1 && warns[0] == "学習クロール失敗 (source=youtube query=\"q\"): yt boom");
        CHECK(r.sufficient);
    }
    {
        // minDensity が不正なら学習せず sufficient=false (TS の DENSITY_RANK[x] = undefined と同じ)
        Growing voices;
        FakeCollect fc{&voices};
        ScriptLlm llm({eval_json("sparse")});
        InformationGateDeps d;
        d.theme = "t";
        d.llm = &llm;
        d.crawlSource = "niconico";
        d.config = base_config(2, 2, "bogus");
        d.collectAndImportFn = fc.fn();
        const auto r = run_information_gate(d);
        CHECK(!r.sufficient && r.crawls == 0 && fc.calls.empty());
    }
}

DI_TEST_MAIN()
