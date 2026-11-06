// w2g_golden のテスト (1/4)。準備は w2g_golden_support.hpp。
#include "w2g_golden_support.hpp"

using namespace w2g_golden_support;

TEST_CASE("paper_markdown: paperDraftToMarkdown") {
    for (const auto& c : G("draftToMarkdown")) CHECK_EQ(paper_draft_to_markdown(paper_content_from_json(c["content"])), c["md"].get<std::string>());
}

TEST_CASE("paper_markdown: markdownToPaperDraft / 固定欄 / stripProgress") {
    const auto& mds = G("fromMarkdown");
    // gen と同じ fallback
    const Json mechs = G("draftToMarkdown")[1]["content"]["mechanics"];
    PaperContent fb;
    fb.theme = "FB";
    fb.tags = {"内部"};
    fb.supplement = "FB補足";
    fb.mechanics = mechs_from(mechs);
    fb.issues = std::vector<std::string>{"FB論点"};
    PaperContent fbf = fb;
    fbf.gameTitle = "FBゲーム";
    fbf.mechanicsContext = "FB文脈";
    for (const auto& c : mds) {
        const std::string md = c["md"];
        W2G_SAME(to_json(markdown_to_paper_draft(md, fb)), c["draft"]);
        W2G_SAME(to_json(markdown_to_paper_draft(md, fbf)), c["draftFixed"]);
        W2G_SAME(to_json(paper_fixed_fields_from_markdown(md, fbf)), c["fixed"]);
        CHECK_EQ(strip_progress(md), c["stripped"].get<std::string>());
    }
}

TEST_CASE("paper_markdown: renderProgressMarkdown") {
    for (const auto& c : G("progress")) {
        std::optional<std::string> conclusion = w2g::opt(c, "conclusion");
        CHECK_EQ(render_progress_markdown(c["base"].get<std::string>(), rounds_from(c["rounds"]), conclusion), c["md"].get<std::string>());
    }
}

TEST_CASE("discussion_paper: ペルソナ配布・system・user プロンプト") {
    for (const auto& c : G("personaPaper")) {
        const Json& a = c["args"];
        BuildPersonaPaperArgs args;
        const Json& p = a["paper"];
        args.paper.paperId = p["paperId"];
        args.paper.sessionId = p["sessionId"];
        args.paper.theme = p["theme"];
        args.paper.tags = w2g::strings(p["tags"]);
        args.paper.mechanics = mechs_from(p["mechanics"]);
        args.paper.supplement = p["supplement"];
        args.paper.rounds = rounds_from(p["rounds"]);
        args.paper.bodyMd = w2g::opt(p, "bodyMd");
        args.persona = persona_from(a["persona"]);
        args.stance = a["stance"];
        for (const auto& u : a["currentRoundUtterances"]) args.currentRoundUtterances.push_back({u["personaName"], u["text"]});
        args.userVoices = w2g::voices_from(a["userVoices"]);
        if (a.contains("syntheticOpinions")) args.syntheticOpinions = w2g::strings(a["syntheticOpinions"]);
        const auto pp = build_persona_paper(args);
        const Json& e = c["paper"];
        CHECK_EQ(pp.mechanicsText, e["mechanicsText"].get<std::string>());
        CHECK_EQ(pp.previousRoundsText, e["previousRoundsText"].get<std::string>());
        CHECK_EQ(pp.supplement, e["supplement"].get<std::string>());
        CHECK(opt_json(pp.userOpinionsText) == (e.contains("userOpinionsText") ? e["userOpinionsText"] : Json(nullptr)));
        CHECK_EQ(build_paper_system(pp), c["system"].get<std::string>());
        CHECK_EQ(build_persona_user_prompt(pp, args.stance, args.persona), c["user"].get<std::string>());
        CHECK_EQ(paper_to_prompt(pp, args.stance, args.persona), c["prompt"].get<std::string>());
    }
    for (const auto& s : G("stanceLines")) CHECK_EQ(stance_line(s[0].get<std::string>()), s[1].get<std::string>());
    for (const auto& t : G("titles")) CHECK_EQ(normalize_discussion_display_title(t[0].get<std::string>()), t[1].get<std::string>());
    W2G_SAME(Json(synthesize_opinions(mechs_from(G("draftToMarkdown")[1]["content"]["mechanics"]))), G("synthetic"));
}

TEST_CASE("LLM 応答の JSON 抽出 (object / array)") {
    for (const auto& c : G("extract")) {
        const std::string raw = c["raw"];
        const auto o = extract_json_object(raw);
        const auto a = extract_json_array(raw);
        W2G_SAME(o ? *o : Json(nullptr), c["object"]);
        W2G_SAME(a ? *a : Json(nullptr), c["array"]);
    }
}

TEST_CASE("effect_predict: normalize / changesToVector / predictEffect") {
    for (const auto& c : G("normalize")) {
        Warns w;
        const auto n = normalize_prediction(c["raw"], w.fn());
        W2G_SAME(n ? to_json(*n) : Json(nullptr), c["result"]);
        W2G_SAME(n ? Json(changes_to_vector(n->changes)) : Json(nullptr), c["vector"]);
        W2G_SAME(w.json(), c["warns"]);
    }
    const auto mechs = mechs_from(G("draftToMarkdown")[1]["content"]["mechanics"]);
    for (const auto& c : G("predict")) {
        LlmCase lc(c);
        PredictEffectArgs a;
        a.opinion = "意見";
        a.theme = "テーマ";
        for (int i = 0; i < 3; ++i) a.mechanics.insert(a.mechanics.end(), mechs.begin(), mechs.end());
        a.model = w2g::opt(c, "model");
        a.warn = lc.warns.fn();
        const auto r = predict_effect(lc.llm, a);
        lc.check(r ? to_json(*r) : Json(nullptr), "predict");
    }
}

int main() { return di::test::run_all(); }
