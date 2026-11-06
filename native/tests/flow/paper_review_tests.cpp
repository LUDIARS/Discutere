// paper_review のテスト (1/2)。準備は paper_review_support.hpp。
#include "paper_review_support.hpp"

using namespace paper_review_support;

// ── golden ─────────────────────────────────────────────────────────────────

TEST_CASE("golden: merge_mechanics (名前で重複排除・先勝ち・全角も小文字化)") {
    const auto& m = G("merge");
    W3O_SAME(mechs_json(merge_mechanics(mechs_from(m["primary"]), mechs_from(m["secondary"]))), m["out"]);
}

TEST_CASE("golden: assess_paper_understanding (決定論 / LLM 成功・失敗・例外)") {
    for (const auto& c : G("understanding")) {
        std::unique_ptr<FakeLLM> llm;
        if (!c["responses"].is_null()) llm = std::make_unique<FakeLLM>(c["responses"]);
        Warns warns;
        AssessUnderstandingOpts opts;
        opts.flow = opt(c, "flow");
        opts.model = opt(c, "model");
        opts.warn = warns.fn();
        const auto r = assess_paper_understanding(fields_from(c["fields"]), mechs_from(c["mechanics"]), voices_from(c["voices"]), llm.get(), opts);
        W3O_SAME(to_json(r), c["result"]);
        W3O_SAME(llm ? llm->calls : Json::array(), c["calls"]);
        W3O_SAME(warns.json(), c["warns"]);
    }
}

TEST_CASE("golden: build_paper_draft (investigate / 議論の data/games / 増補 / 理解確認 / 議論適性ゲート / 機密)") {
    TempDir dir;
    const std::string games = dir.path + "/games";
    fs::ensure_dir(games);
    CHECK(fs::write_file_atomic(games + "/monster-strike.md", G("gameMd").get<std::string>()));
    for (const auto& c : G("build")) {
        const std::string name = c["name"];
        std::unique_ptr<FakeLLM> llm, ullm, gate;
        if (c.contains("llm")) llm = std::make_unique<FakeLLM>(c["llm"]);
        if (c.contains("understandingLlm")) ullm = std::make_unique<FakeLLM>(c["understandingLlm"]);
        if (c.contains("gate")) gate = std::make_unique<FakeLLM>(c["gate"]["responses"]);
        Json list_calls = Json::array();
        Warns warns;
        BuildPaperDraftDeps deps;
        deps.flow = opt(c, "flow");
        deps.gamesDir = c["useGames"].get<bool>() ? games : dir.path + "/no-such-games";
        if (c.contains("voices")) deps.listExternalVoices = fixed_voices(c["voices"], &list_calls);
        deps.llm = llm.get();
        deps.understandingLlm = ullm.get();
        if (c.contains("mechanicsTarget")) deps.mechanicsTarget = c["mechanicsTarget"].get<int>();
        deps.enrichModel = opt(c, "enrichModel");
        if (c.contains("extra")) deps.extraMechanics = mechs_from(c["extra"]);
        if (c.contains("seed")) deps.seed = fields_from(c["seed"]);
        if (gate) deps.debatability = PaperDebatabilityGate{gate.get(), c["gate"]["min"].get<int>(), {}};
        deps.warn = warns.fn();
        std::vector<FlowTag> tags = w3o::strings(c["tags"]);
        const auto r = build_paper_draft(c["theme"], tags, deps);
        if (!w3o::same(to_json(r.draft), c["draft"])) throw test::Failure(name + " draft\n  actual: " + to_json(r.draft).dump() + "\n  expected: " + c["draft"].dump());
        if (!w3o::same(to_json(r.info), c["info"])) throw test::Failure(name + " info\n  actual: " + to_json(r.info).dump() + "\n  expected: " + c["info"].dump());
        W3O_SAME(llm ? llm->calls : Json::array(), c["llmCalls"]);
        W3O_SAME(ullm ? ullm->calls : Json::array(), c["understandingCalls"]);
        W3O_SAME(gate ? gate->calls : Json::array(), c["gateCalls"]);
        W3O_SAME(list_calls, c["listCalls"]);
    }
}

TEST_CASE("golden: apply_paper_edit (反映・フェンス混入・パース不能・LLM 失敗/例外・空指示・固定欄)") {
    for (const auto& c : G("edit")) {
        FakeLLM llm(c["responses"]);
        Warns warns;
        ApplyPaperEditOpts opts;
        opts.model = opt(c, "model");
        opts.warn = warns.fn();
        const auto r = apply_paper_edit(paper_draft_from_json(c["draft"]), c["instruction"], llm, opts);
        W3O_SAME(to_json(r), c["result"]);
        W3O_SAME(llm.calls, c["calls"]);
        W3O_SAME(warns.json(), c["warns"]);
    }
}

TEST_CASE("golden: coerce_paper_draft (不正入力・本文 md 優先・タグの絞り込み)") {
    const auto& g = G("coerce");
    const auto fallback = paper_draft_from_json(g["fallback"]);
    for (const auto& c : g["cases"]) W3O_SAME(to_json(coerce_paper_draft(c["raw"], fallback)), c["result"]);
}

TEST_CASE("golden: render_paper_review") {
    for (const auto& c : G("render")) CHECK_EQ(render_paper_review(paper_draft_from_json(c["draft"]), info_from(c["info"])), c["md"].get<std::string>());
}

TEST_CASE("golden: is_approval_text / is_revert_text") {
    for (const auto& c : G("words")) {
        const std::string t = c["text"];
        if (is_approval_text(t) != c["approval"].get<bool>()) throw test::Failure("approval: " + t);
        if (is_revert_text(t) != c["revert"].get<bool>()) throw test::Failure("revert: " + t);
    }
}

TEST_CASE("golden: review_block") {
    const auto& g = G("review");
    for (const auto& c : g["cases"]) {
        FakeLLM llm(c["responses"]);
        Warns warns;
        ReviewBlockArgs args;
        args.bodyMd = g["bodyMd"];
        args.blockId = c["blockId"];
        args.instruction = opt(c, "instruction");
        args.model = opt(c, "model");
        args.warn = warns.fn();
        W3O_SAME(to_json(review_block(llm, args)), c["result"]);
        W3O_SAME(llm.calls, c["calls"]);
        W3O_SAME(warns.json(), c["warns"]);
    }
}

DI_TEST_MAIN()
