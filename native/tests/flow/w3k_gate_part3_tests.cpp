// w3k_gate のテスト (3/4)。準備は w3k_gate_support.hpp。
#include "w3k_gate_support.hpp"

using namespace w3k_gate_support;

TEST_CASE("density_features: フラット特徴量") {
    auto voices = args_of(3).voices;
    for (const auto& v : args_of(2, "youtube").voices) voices.push_back(v);
    const auto f = density_features(voices, {"機密"});
    CHECK_EQ(f["voiceCount"].get<int>(), 5);
    CHECK_EQ(f["sourceKinds"].get<int>(), 2);
    CHECK_EQ(f["avgLen"].get<int>(), 10);
    CHECK_EQ(f["tagCount"].get<int>(), 1);
    CHECK_EQ(density_features({}, {})["avgLen"].get<int>(), 0);
}

TEST_CASE("blackbox: LLM 経路は完全な評価を透過・影評価で trial へ・条件不一致は LLM・不正な提案は登録しない") {
    {
        auto bb = lib::blackbox::make_memory_blackbox();
        int calls = 0;
        auto evaluate = make_blackbox_density_evaluator(bb, fake_evaluate(&calls));
        const auto ev = evaluate(args_of(10));
        CHECK_EQ(ev.density, std::string("rich"));
        CHECK((ev.covered == std::vector<std::string>{"肯定", "否定"}));
        CHECK_EQ(calls, 1);
        const auto rules = bb.rules->list_by_domain(DOMAIN_DENSITY);
        CHECK(rules.size() == 1 && rules[0].state == "candidate");
    }
    {
        auto bb = lib::blackbox::make_memory_blackbox();
        int calls = 0;
        auto evaluate = make_blackbox_density_evaluator(bb, fake_evaluate(&calls));
        for (int i = 0; i < 4; ++i) evaluate(args_of(10));
        CHECK_EQ(bb.rules->list_by_domain(DOMAIN_DENSITY)[0].state, std::string("trial"));
        CHECK_EQ(calls, 4);
        const auto ev = evaluate(args_of(12));
        CHECK_EQ(calls, 4);
        CHECK_EQ(ev.density, std::string("rich"));
        CHECK(ev.gaps.empty());
        evaluate(args_of(2));  // when 不一致
        CHECK_EQ(calls, 5);
    }
    {
        auto bb = lib::blackbox::make_memory_blackbox();
        auto evaluate = make_blackbox_density_evaluator(bb, [](const EvaluateArgs&) {
            InformationEvaluation e;
            e.density = "moderate";
            e.proposedRule = Json{{"when", Json{{"op", "bogus"}}}};
            return e;
        });
        CHECK_EQ(evaluate(args_of(4)).density, std::string("moderate"));
        CHECK(bb.rules->list_by_domain(DOMAIN_DENSITY).empty());
    }
    // parse_proposed_rule の境界
    CHECK(!parse_proposed_rule(std::nullopt, "rich"));
    CHECK(!parse_proposed_rule(Json("x"), "rich"));
    CHECK(!parse_proposed_rule(Json::array(), "rich"));
    const auto p = parse_proposed_rule(Json{{"when", Json{{"op", "cmp"}, {"feature", "voiceCount"}, {"cmp", ">="}, {"value", 3}}}}, "moderate");
    CHECK(p && p->description == "密度 moderate の閾値ルール" && p->confidence == 0.7 && p->output["density"] == "moderate");
    // 深すぎる条件は validate_condition が拒否
    Json deep = Json{{"op", "cmp"}, {"feature", "voiceCount"}, {"cmp", ">="}, {"value", 1}};
    for (int i = 0; i < 20; ++i) deep = Json{{"op", "not"}, {"clause", deep}};
    CHECK(!parse_proposed_rule(Json{{"when", deep}}, "rich"));
}

TEST_CASE("blackbox: ルールの output が不正なら sparse (安全側)") {
    auto bb = lib::blackbox::make_memory_blackbox();
    lib::blackbox::RuleDraft draft;
    draft.domain = DOMAIN_DENSITY;
    draft.description = "壊れた出力";
    draft.when = Json{{"op", "cmp"}, {"feature", "voiceCount"}, {"cmp", ">="}, {"value", 1}};
    draft.output = Json{{"density", "huge"}};
    bb.engine->add_rule(draft);  // manual は trial から
    int calls = 0;
    auto evaluate = make_blackbox_density_evaluator(bb, fake_evaluate(&calls));
    CHECK_EQ(evaluate(args_of(3)).density, std::string("sparse"));
    CHECK_EQ(calls, 0);
}

TEST_CASE("autocrawl: ヘルパ") {
    CHECK(is_auto_crawl_source("niconico") && is_auto_crawl_source("reddit") && !is_auto_crawl_source("x"));
    CHECK_EQ(derive_slug("Hello World"), std::string("hello-world"));
    CHECK_EQ(derive_slug("ローグライト"), std::string("ローグライト"));
    CHECK_EQ(derive_slug("   "), std::string("discussion"));
    CHECK_EQ(count_learning_voices("t", voices_of(4), 3), 4);
    CHECK_EQ(count_learning_voices("t", {}, 3), 0);
    CHECK_EQ(count_learning_voices("t", [](const std::vector<std::string>&, int) -> std::vector<ContextVoice> { throw std::runtime_error("x"); }, 3), 0);
    CollectorContext ctx;
    ctx.theme = "テーマ";
    ctx.spec.query = "  ";
    CHECK_EQ(resolve_query(ctx), std::string("テーマ"));
    ctx.spec.query = " q ";
    CHECK_EQ(resolve_query(ctx), std::string("q"));
}

TEST_CASE("autocrawl: 充足 → skip / 不足 → クロール → 取込 / 0 件 / listExternalVoices 未指定 / 未対応ソース") {
    {
        bool called = false;
        EnsureLearningDataDeps d;
        d.theme = "ローグライト";
        d.slug = "rogue";
        d.workspaceId = "knowledge";
        d.spec.source = "niconico";
        d.minVoices = 3;
        d.listExternalVoices = voices_of(5);
        d.collectors["niconico"] = [&](const CollectorContext&) {
            called = true;
            return std::vector<cs::ExternalUtterance>{};
        };
        d.importItems = [](core::Core*, const std::vector<cs::ExternalUtterance>&, const std::string&) { return 0; };
        const auto r = ensure_learning_data(d);
        CHECK(r.skipped && !r.crawled && r.voicesBefore == 5 && !called);
        CHECK_EQ(r.message, std::string("学習データ充分 (外部の声 5 件 ≥ 3)"));
    }
    {
        std::string saw;
        int imported = 0;
        EnsureLearningDataDeps d;
        d.theme = "周回設計";
        d.slug = "shuukai";
        d.spec.source = "niconico";
        d.minVoices = 3;
        d.listExternalVoices = voices_of(1);
        d.collectors["niconico"] = [&](const CollectorContext& ctx) {
            saw = resolve_query(ctx);
            CHECK_EQ(ctx.gameSlug, std::string("shuukai"));
            CHECK_EQ(ctx.maxItems, 200);
            return fake_items(7);
        };
        d.importItems = [&](core::Core*, const std::vector<cs::ExternalUtterance>& items, const std::string&) {
            imported = static_cast<int>(items.size());
            return imported;
        };
        const auto r = ensure_learning_data(d);
        CHECK(!r.skipped && r.crawled && r.imported == 7 && imported == 7);
        CHECK_EQ(saw, std::string("周回設計"));
        CHECK_EQ(r.message, std::string("niconico で 7 件取得 → 7 件を学習データに追加"));
    }
    {
        EnsureLearningDataDeps d;
        d.theme = "無人気テーマ";
        d.spec.source = "niconico";
        d.minVoices = 3;
        d.listExternalVoices = voices_of(0);
        d.collectors["niconico"] = [](const CollectorContext&) { return std::vector<cs::ExternalUtterance>{}; };
        d.importItems = [](core::Core*, const std::vector<cs::ExternalUtterance>&, const std::string&) { return 0; };
        const auto r = ensure_learning_data(d);
        CHECK(!r.crawled && !r.skipped && r.imported == 0);
    }
    {
        EnsureLearningDataDeps d;
        d.theme = "t";
        d.spec.source = "niconico";
        d.minVoices = 3;
        d.collectors["niconico"] = [](const CollectorContext&) { return fake_items(2); };
        d.importItems = [](core::Core*, const std::vector<cs::ExternalUtterance>& items, const std::string&) { return static_cast<int>(items.size()); };
        const auto r = ensure_learning_data(d);
        CHECK(r.voicesBefore == 0 && r.crawled && r.imported == 2);
    }
    {
        std::vector<std::string> warns;
        EnsureLearningDataDeps d;
        d.theme = "t";
        d.spec.source = "bogus";
        d.minVoices = 3;
        d.warn = [&](const std::string& m) { warns.push_back(m); };
        const auto r = ensure_learning_data(d);
        CHECK_EQ(r.message, std::string("未対応のソース: bogus"));
        CHECK(warns.size() == 1);
    }
}

DI_TEST_MAIN()
