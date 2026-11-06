// paper_review のテスト (2/2)。準備は paper_review_support.hpp。
#include "paper_review_support.hpp"

using namespace paper_review_support;

TEST_CASE("golden: gather_evidence") {
    for (const auto& c : G("evidence")) {
        std::unique_ptr<FakeLLM> llm;
        if (!c["responses"].is_null()) llm = std::make_unique<FakeLLM>(c["responses"]);
        Json list_calls = Json::array();
        Warns warns;
        GatherEvidenceArgs args;
        args.topic = c["topic"];
        args.listExternalVoices = fixed_voices(c["voices"], &list_calls);
        args.llm = llm.get();
        args.model = opt(c, "model");
        if (c.contains("limit")) args.limit = c["limit"].get<int>();
        args.warn = warns.fn();
        W3O_SAME(to_json(gather_evidence(args)), c["result"]);
        W3O_SAME(llm ? llm->calls : Json::array(), c["calls"]);
        W3O_SAME(list_calls, c["listCalls"]);
        W3O_SAME(warns.json(), c["warns"]);
    }
}

TEST_CASE("golden: build_intent_instruction (調整 / 外部の声 / 類似ゲーム)") {
    for (const auto& c : G("enrich")) {
        const auto& in = c["intent"];
        PaperReviewIntent intent{in["kind"], in["text"], w3o::strings(in["terms"])};
        std::unique_ptr<FakeLLM> llm;
        if (c.contains("llm")) llm = std::make_unique<FakeLLM>(c["llm"]);
        Json crawl_calls = Json::array();
        Json inv_calls = Json::array();
        Warns warns;
        PaperIntentDeps deps;
        deps.theme = "議題X";
        deps.tags = {"開発"};
        deps.gamesDir = "/no/such";
        if (c.contains("voices") && !c["voices"].is_null()) deps.listExternalVoices = fixed_voices(c["voices"]);
        deps.llm = llm.get();
        if (c.contains("crawl")) {
            const Json crawl = c["crawl"];
            deps.crawl = [crawl, &crawl_calls](const std::string& q) {
                crawl_calls.push_back(q);
                if (crawl.is_string()) throw std::runtime_error("crawl down");
                return crawl.get<int>();
            };
        }
        const Json inv = c.contains("investigate") ? c["investigate"] : Json::object();
        deps.investigate = [inv, &inv_calls](const InvestigateArgs& a) {
            inv_calls.push_back(Json{{"theme", a.theme}, {"tags", a.tags}, {"gamesDir", a.gamesDir}});
            InvestigateResult r;
            if (inv.contains(a.theme)) {
                if (inv[a.theme].is_string()) throw std::runtime_error("inv down");
                r.mechanics = mechs_from(inv[a.theme]);
            }
            return r;
        };
        deps.warn = warns.fn();
        W3O_SAME(to_json(build_intent_instruction(intent, deps)), c["result"]);
        W3O_SAME(crawl_calls, c["crawlCalls"]);
        W3O_SAME(inv_calls, c["invCalls"]);
        W3O_SAME(warns.json(), c["warns"]);
        W3O_SAME(llm ? llm->calls : Json::array(), c["calls"]);
    }
}

// ── 移した TS テスト ───────────────────────────────────────────────────────

TEST_CASE("paper-review.test: 企画段階は既存タイトルを要求しない / 改善は要求する") {
    PaperFixedFields seed{"", "短時間で協力を楽しむ企画", "役割分担案を比較する", "", ""};
    int searches = 0;
    BuildPaperDraftDeps deps;
    deps.flow = "discussion";
    deps.seed = seed;
    deps.gamesDir = "/no/such/games-dir";
    deps.youtubeSearch = [&](const std::string&, int) {
        ++searches;
        return std::vector<std::string>{};
    };
    const auto r = build_paper_draft(seed.discussionTheme, {}, deps);
    CHECK_EQ(searches, 0);
    CHECK_EQ(r.draft.gameTitle.value_or("?"), std::string(""));
    CHECK(r.draft.mechanics.empty());
    CHECK(r.info.understanding && r.info.understanding->ok);
    AssessUnderstandingOpts o;
    o.flow = "improvement";
    CHECK(!assess_paper_understanding(seed, {}, {}, nullptr, o).ok);
}

TEST_CASE("paper-review.test: apply_paper_edit / coerce / 承認語 / md 往復 / review_block / gather_evidence") {
    // パース不能なら元のまま
    auto base = with_derived_body(PaperContent{"T", {"開発"}, "S", {MechanicSummary{"M", "d", {}, {}, {}, {}, {}}}, {}, {}, {}, {}, {}, {}});
    persona_engine::MockLLMClient bad({[](const persona_engine::LLMInvokeArgs&) { return std::variant<std::string, persona_engine::LLMResult>(std::string("ごめんなさい")); }});
    const auto r = apply_paper_edit(base, "曖昧な指示", bad);
    CHECK(!r.applied);
    W3O_SAME(to_json(r.draft), to_json(base));
    // 不正入力はまるごと fallback
    W3O_SAME(to_json(coerce_paper_draft(Json(nullptr), base)), to_json(base));
    for (const char* t : {"開始", "承認", "OK", "ok。", "これで開始", "✅", "go"}) CHECK(is_approval_text(t));
    for (const char* t : {"", "メカニクスにガチャを追加", "もっと否定的に", "開始したい理由は"}) CHECK(!is_approval_text(t));
    for (const char* t : {"戻す", "戻して", "undo", "revert", "↶", "取り消し"}) CHECK(is_revert_text(t));
    for (const char* t : {"", "開始", "メカニクスを戻して具体化", "もっと簡単に"}) CHECK(!is_revert_text(t));
    // ハイブリッド源泉: ドラフト ⇄ 本文 md の往復
    auto draft = with_derived_body(PaperContent{"スイカゲームの面白さ", {"開発"}, "開発者観点で",
                                                {MechanicSummary{"落下", "果物を落とす", std::string("爽快"), {}, {}, {}, {}}}, {}, {}, {}, {}, {}, {}});
    CHECK(str::contains(draft.bodyMd, "# 議題"));
    CHECK(str::contains(draft.bodyMd, "- **落下**: 果物を落とす → 期待感情: 爽快"));
    const auto back = with_derived_structure(draft.bodyMd, draft);
    CHECK_EQ(back.theme, std::string("スイカゲームの面白さ"));
    CHECK(back.tags == std::vector<FlowTag>{"開発"});
    CHECK_EQ(back.mechanics.size(), 1u);
    CHECK_EQ(back.mechanics[0].intended_affect.value_or(""), std::string("爽快"));
    // review_block: 不在ブロックは ok=false
    FakeLLM llm(Json::array({Json{{"ok", true}, {"text", R"({"proposed":"x","rationale":"y"})"}}}));
    ReviewBlockArgs ra;
    ra.bodyMd = draft.bodyMd;
    ra.blockId = "b999";
    CHECK(!review_block(llm, ra).ok);
    CHECK_EQ(llm.calls.size(), 0u);
    // gather_evidence: 声ゼロは空 suggestion
    GatherEvidenceArgs ga;
    ga.topic = "無";
    ga.listExternalVoices = [](const std::vector<std::string>&, int) { return std::vector<ContextVoice>{}; };
    CHECK_EQ(gather_evidence(ga).suggestion, std::string(""));
}

TEST_CASE("paper-review-discussion-mechanics.test: 議論は data/games のメカニクスだけ (外部収集・増補なし)") {
    TempDir dir;
    CHECK(fs::write_file_atomic(dir.path + "/monster-strike.md", G("gameMd").get<std::string>()));
    CHECK(fs::write_file_atomic(dir.path + "/other-game.md",
                                "---\nid: other-game\ntitle: 別のゲーム\nmechanics:\n  - name: 無関係\n    description: 混ざってはいけない\n---\n"));
    FakeLLM spy(Json::array({Json{{"ok", true}, {"text", R"({"mechanics":[{"name":"LLM増補","description":"仮説"}]})"}}}));
    int searches = 0;
    BuildPaperDraftDeps deps;
    deps.flow = "discussion";
    deps.seed = PaperFixedFields{"モンスターストライク", "長寿コンテンツとして遊ばれ続ける理由", "", "", ""};
    deps.gamesDir = dir.path;
    deps.llm = &spy;
    deps.youtubeSearch = [&](const std::string&, int) {
        ++searches;
        return std::vector<std::string>{};
    };
    FakeLLM ok_llm(Json::array({Json{{"ok", true}, {"text", R"({"ok":true})"}}}));
    deps.understandingLlm = &ok_llm;
    const auto r = build_paper_draft("長寿コンテンツとして遊ばれ続ける理由", {}, deps);
    CHECK_EQ(r.draft.mechanics.size(), 2u);
    CHECK_EQ(r.draft.mechanics[0].name, std::string("ギミック"));
    CHECK_EQ(searches, 0);
    CHECK_EQ(spy.calls.size(), 0u);
    CHECK(!str::contains(r.draft.bodyMd, "無関係"));
    // 事前情報 (extraMechanics) が先勝ち
    deps.extraMechanics = {MechanicSummary{"ギミック", "使い回し由来", {}, {}, {}, {}, std::string("curated")}};
    const auto r2 = build_paper_draft("長寿コンテンツとして遊ばれ続ける理由", {}, deps);
    CHECK_EQ(r2.draft.mechanics[0].description, std::string("使い回し由来"));
    // 一致しないゲーム名 → 空
    deps.extraMechanics.clear();
    deps.seed = PaperFixedFields{"存在しない新作", "x", "", "", ""};
    CHECK(build_paper_draft("x", {}, deps).draft.mechanics.empty());
}

TEST_CASE("機密/内部タグでは蓄積済みの外部の声もペーパーに載せない") {
    bool called = false;
    BuildPaperDraftDeps deps;
    deps.gamesDir = "/no/such";
    deps.listExternalVoices = [&](const std::vector<std::string>&, int) {
        called = true;
        return std::vector<ContextVoice>{{"声", "steam", {}}};
    };
    for (const char* tag : {"機密", "内部"}) {
        const auto r = build_paper_draft("T", {tag}, deps);
        CHECK(!called);
        CHECK_EQ(r.info.voiceCount, 0);
        CHECK(r.info.samples.empty());
    }
    CHECK_EQ(build_paper_draft("T", {"開発"}, deps).info.voiceCount, 1);
    CHECK(called);
}

TEST_CASE("境界: apply_paper_edit の LLM JSON は上限ちょうどまで読み、+1 / 壊れた / 深すぎる応答は反映しない") {
    const auto base = with_derived_body(PaperContent{"T", {}, "S", {}, {}, {}, {}, {}, {}, {}});
    const std::size_t limit = llm_json_limits().max_bytes;
    {
        FakeLLM llm(Json::array({Json{{"ok", true}, {"text", edit_response_of_size(limit)}}}));
        CHECK(apply_paper_edit(base, "x", llm).applied);
    }
    {
        FakeLLM llm(Json::array({Json{{"ok", true}, {"text", edit_response_of_size(limit + 1)}}}));
        const auto r = apply_paper_edit(base, "x", llm);
        CHECK(!r.applied);
        W3O_SAME(to_json(r.draft), to_json(base));
    }
    {
        FakeLLM llm(Json::array({Json{{"ok", true}, {"text", "{\"theme\": \"x\", \"mechanics\": [ {"}}}));
        CHECK(!apply_paper_edit(base, "x", llm).applied);
    }
    {
        // 入れ子の深さの上限 (64) を超える応答
        std::string deep = "{\"a\":";
        for (int i = 0; i < 100; ++i) deep += "[";
        for (int i = 0; i < 100; ++i) deep += "]";
        deep += "}";
        FakeLLM llm(Json::array({Json{{"ok", true}, {"text", deep}}}));
        CHECK(!apply_paper_edit(base, "x", llm).applied);
    }
    {
        // 巨大な応答 (上限の 4 倍) でも落ちずに元のまま
        FakeLLM llm(Json::array({Json{{"ok", true}, {"text", edit_response_of_size(limit * 4)}}}));
        CHECK(!apply_paper_edit(base, "x", llm).applied);
    }
}

TEST_CASE("境界: assess_paper_understanding / review_block も上限超過は決定論・元のままに倒す") {
    const std::size_t limit = llm_json_limits().max_bytes;
    const std::string pad(limit, ' ');
    PaperFixedFields f{"G", "T", "C", "M", ""};
    FakeLLM llm(Json::array({Json{{"ok", true}, {"text", R"({"ok":false,"missingQuestions":["q"]})" + pad}}}));
    // 末尾の空白は `}` の外なので切り出しに含まれず読める (TS と同じ)。
    CHECK(!assess_paper_understanding(f, {}, {}, &llm).ok);
    FakeLLM big(Json::array({Json{{"ok", true}, {"text", R"({"ok":false,"rationale":")" + pad + R"("})"}}}));
    CHECK(assess_paper_understanding(f, {}, {}, &big).ok);  // 上限超過 → 決定論 (全欄あり = ok)
    const auto draft = with_derived_body(PaperContent{"T", {}, "", {}, {}, {}, {}, {}, {}, {}});
    const auto blocks = split_blocks(draft.bodyMd);
    ReviewBlockArgs ra;
    ra.bodyMd = draft.bodyMd;
    ra.blockId = blocks.back().id;
    FakeLLM rb(Json::array({Json{{"ok", true}, {"text", R"({"proposed":")" + pad + R"(x"})"}}}));
    CHECK(!review_block(rb, ra).ok);
}

DI_TEST_MAIN()
