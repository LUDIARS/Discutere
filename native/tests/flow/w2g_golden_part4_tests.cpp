// w2g_golden のテスト (4/4)。準備は w2g_golden_support.hpp。
#include "w2g_golden_support.hpp"

using namespace w2g_golden_support;

TEST_CASE("dialectic: LLM 経路 (偽の LLM に同じ応答を与えた結果)") {
    const Json& L = G("llmDialectic");
    const auto issue = w2g::issue_from(IN("issue"));
    const auto posA = w2g::position_from(IN("posA"));
    const auto posB = w2g::position_from(IN("posB"));
    const auto t0 = w2g::tension_from(IN("tensions")[0]);

    {
        LlmCase c(L["decompose"][0]);
        DecomposeIssuesArgs a{"T", std::string(" # 本文 "), std::string("m"), ISSUES_MAX, {}};
        c.check(Json(decompose_issues(c.llm, a)), "decompose0");
        LlmCase c1(L["decompose"][1]);
        c1.check(Json(decompose_issues(c1.llm, {"T", std::nullopt, std::nullopt, 0, {}})), "decompose1");
        for (int i = 2; i < 4; ++i) {
            LlmCase ci(L["decompose"][static_cast<std::size_t>(i)]);
            std::string err;
            try {
                decompose_issues(ci.llm, {"T", std::nullopt, std::nullopt, ISSUES_MAX, {}});
            } catch (const std::exception& ex) {
                err = ex.what();
            }
            CHECK_EQ(err, ci.c["error"].get<std::string>());
            ci.check(Json(nullptr), "decompose-err");
        }
    }
    {
        const std::vector<std::tuple<int, std::string, std::optional<std::string>>> args{
            {2, "con", std::string("mm")}, {0, "pro", {}}, {2, "con", {}}, {1, "pro", {}}, {0, "pro", {}}, {0, "pro", {}}};
        for (std::size_t i = 0; i < args.size(); ++i) {
            LlmCase c(L["position"][i]);
            GeneratePositionArgs a;
            a.theme = "T";
            a.issue = issue;
            a.persona = P(std::get<0>(args[i]));
            a.stance = std::get<1>(args[i]);
            a.paperSystem = "SYS";
            a.model = std::get<2>(args[i]);
            a.warn = c.warns.fn();
            c.check(to_json(generate_position(c.llm, a)), "position");
        }
    }
    {
        LlmCase c0(L["stance"][0]);
        c0.check(Json(judge_position_stance(c0.llm, {issue, "c", std::nullopt, std::string("j"), c0.warns.fn()})), "stance0");
        LlmCase c1(L["stance"][1]);
        c1.check(Json(judge_position_stance(c1.llm, {issue, "c", std::string("q"), std::nullopt, c1.warns.fn()})), "stance1");
    }
    {
        auto gate_json = [](const RebutGateResult& r) {
            Json o = Json::object();
            o["accepted"] = r.accepted;
            if (r.accepted) o["ground"] = r.ground ? to_json(*r.ground) : Json(nullptr);
            else o["foul"] = r.foul;
            return o;
        };
        struct G2 {
            std::optional<std::string> attack, gid;
            bool own;
            bool judge;
            std::optional<std::string> model;
        };
        const std::vector<G2> cases{{"data", "pro-G1", true, true, "j"}, {"qualifier", {}, false, true, {}}, {"data", "pro-G1", false, true, {}},
                                    {"data", "pro-G1", false, false, {}}, {"data", "zz", false, true, {}}};
        for (std::size_t i = 0; i < cases.size(); ++i) {
            LlmCase c(L["gate"][i]);
            GateRebutArgs a;
            a.claim = {cases[i].attack, cases[i].gid, std::string("d"), std::string("w")};
            a.issue = issue;
            a.position = posA;
            if (cases[i].own) a.ownPosition = posB;
            a.rebutText = "t";
            a.judge = cases[i].judge;
            a.model = cases[i].model;
            a.warn = c.warns.fn();
            c.check(gate_json(gate_rebut(c.llm, a)), "gate");
        }
    }
    for (std::size_t i = 0; i < 4; ++i) {
        LlmCase c(L["concession"][i]);
        c.check(Json(judge_concession(c.llm, "a", "b", i == 0 ? std::optional<std::string>("j") : std::nullopt, c.warns.fn())), "concession");
    }
    for (std::size_t i = 0; i < 3; ++i) {
        LlmCase c(L["classify"][i]);
        c.check(Json(classify_tension(c.llm, {issue, posA, posB, i == 0 ? std::optional<std::string>("j") : std::nullopt, c.warns.fn()})), "classify");
    }
    {
        const std::vector<ListExternalVoicesFn> lookups{
            [](const std::vector<std::string>& t, int n) { return std::vector<ContextVoice>{{str::join(t, "|") + std::to_string(n), "s", {}}}; },
            [](const std::vector<std::string>&, int) { return std::vector<ContextVoice>{{"c", "s", {}}}; },
            [](const std::vector<std::string>&, int) { return std::vector<ContextVoice>{{"c", "s", {}}}; },
            [](const std::vector<std::string>&, int) -> std::vector<ContextVoice> { throw std::runtime_error("kg down"); }};
        for (std::size_t i = 0; i < lookups.size(); ++i) {
            LlmCase c(L["fact"][i]);
            const auto r = resolve_fact_tension(c.llm, {"T", issue, posA, posB, lookups[i], std::nullopt, c.warns.fn()});
            c.check(Json({{"status", r.status}, {"note", r.note}}), "fact");
        }
    }
    {
        std::string long_both = "Both の上位目標を再定義して条件分割で両立させる長い説明";
        std::string frame = "新しい枠の説明がここにあり十分な長さを持つ文";
        const std::vector<std::pair<std::optional<std::string>, std::optional<std::string>>> el{
            {"  ", {}}, {"両方やる", {}}, {long_both, "j"}, {frame, {}}, {frame, {}}, {frame, {}}};
        for (std::size_t i = 0; i < el.size(); ++i) {
            LlmCase c(L["elevation"][i]);
            c.check(Json(elevation_gate(c.llm, el[i].first, el[i].second, c.warns.fn())), "elevation");
        }
    }
    {
        const std::vector<std::tuple<PositionRecord, std::string, std::vector<std::string>, std::optional<std::string>, std::optional<std::string>>> rs{
            {posA, "A", {"pro-G1"}, {}, {}}, {posA, "A", {}, "e", "j"}, {posA, "A", {}, {}, {}}, {posB, "C", {}, {}, {}}};
        for (std::size_t i = 0; i < rs.size(); ++i) {
            LlmCase c(L["ratify"][i]);
            RatifyArgs a;
            a.position = std::get<0>(rs[i]);
            a.personaName = std::get<1>(rs[i]);
            a.synthesisText = "s";
            a.preserves = std::get<2>(rs[i]);
            a.elevates = std::get<3>(rs[i]);
            a.model = std::get<4>(rs[i]);
            a.warn = c.warns.fn();
            c.check(to_json(ratify_synthesis(c.llm, a)), "ratify");
        }
    }
    for (std::size_t i = 0; i < 3; ++i) {
        LlmCase c(L["generate"][i]);
        GenerateSynthesisArgs a;
        a.issueTitle = "I";
        a.tension = t0;
        a.positionA = posA;
        a.positionB = posB;
        a.paperSystem = "SYS";
        if (i == 0) a.model = "mm";
        a.warn = c.warns.fn();
        const auto r = generate_synthesis(c.llm, a);
        c.check(Json({{"text", r.text}, {"preserves", r.preserves}, {"negates", r.negates}, {"elevates", opt_json(r.elevates)}, {"degraded", r.degraded}}),
                "generate");
    }
    {
        // gen と同じ d1 / d3
        ArgumentGraph g;
        g.apply({"c1", "p1", "claim", std::nullopt, 1});
        g.apply({"c2", "p3", "claim", std::nullopt, 2});
        g.apply({"r1", "p3", "rebut", std::string("c1"), 3});
        const auto d1 = G("scheduler")["decisions"][0];
        const auto d3 = G("scheduler")["decisions"][2];
        auto decision = [](const Json& d) {
            return ScheduleDecision{d["kind"], d["personaId"], w2g::opt(d, "targetUtteranceId"), w2g::strings(d["allowedActs"]), d["reason"]};
        };
        const UtteranceDigest tgt{IN("tgt")["id"], IN("tgt")["personaName"], IN("tgt")["text"]};
        LlmCase c0(L["nomination"][0]);
        c0.check(Json(render_nomination(c0.llm, {decision(d1), "A", tgt, std::string("f"), c0.warns.fn()})), "nomination0");
        LlmCase c1(L["nomination"][1]);
        c1.check(Json(render_nomination(c1.llm, {decision(d3), "A", std::nullopt, std::nullopt, c1.warns.fn()})), "nomination1");
        LlmCase c2(L["nomination"][2]);
        c2.check(Json(render_facilitator_line(c2.llm, "内容", std::nullopt, c2.warns.fn())), "facilitator");
    }
}

TEST_CASE("proxy: 代理の定立の帰属検査") {
    const Json& e = G("positionAttribution");
    GeneratedPosition gp;
    const Json& in = e["input"];
    gp.claim = in["claim"];
    gp.text = in["text"];
    gp.groundTexts = w2g::strings(in["groundTexts"]);
    for (const auto& g : in["grounds"]) gp.grounds.push_back(ground_from_json(g));
    Warns w;
    W2G_SAME(to_json(proxy::attribute_position_grounds(gp, persona_from(e["persona"]), w.fn())), e["result"]);
    W2G_SAME(w.json(), e["warns"]);
    W2G_SAME(to_json(proxy::attribute_position_grounds(gp, P(0))), e["plain"]);
}

TEST_CASE("DB を使う経路 (投票・価値軸・ラウンドまとめ・リファイン・止揚ループ) の呼び出しと結果") {
    w2g::TempFlowDb tmp;
    const Json& V = G("voteRun");
    {
        FakeLLM llm(Json::array({Json{{"ok", true}, {"text", "u2"}}, Json{{"ok", true}, {"text", "2"}}, Json{{"ok", false}, {"error", "e"}},
                                 Json{{"ok", true}, {"text", "??"}}, Json{{"ok", true}, {"text", "[ID:u1] u1"}}}));
        Warns w;
        auto seed = std::make_shared<std::uint64_t>(7);
        RunRoundVoteArgs a;
        a.theme = "T";
        a.sessionId = "VS";
        a.round = 1;
        a.voterCount = 5;
        a.utterances = {{"u1", "A", "t1"}, {"u2", "B", "t2"}, {"u3", "C", "t3"}};
        a.rng = [seed] {
            *seed = (*seed * 1664525ULL + 1013904223ULL) % 4294967296ULL;
            return static_cast<double>(*seed) / 4294967296.0;
        };
        a.warn = w.fn();
        W2G_SAME(to_json(run_round_vote(llm, a)), V["results"][0]);
        W2G_SAME(llm.calls, V["calls"]);
        W2G_SAME(w.json(), V["warns"]);
        FakeLLM none(Json::array());
        RunRoundVoteArgs b;
        b.sessionId = "VS";
        b.voterCount = 3;
        W2G_SAME(to_json(run_round_vote(none, b)), V["results"][1]);
        CHECK_EQ(none.calls.size(), std::size_t(0));
        // 5 票ぶんの vote 行 (棄権は NULL)
        const auto rows = db::get_flow_db()->all("SELECT chosen_utterance_id FROM vote WHERE session_id = 'VS' ORDER BY voter_index");
        CHECK_EQ(rows.size(), std::size_t(5));
        CHECK(rows[2].is_null("chosen_utterance_id"));
    }
    {
        const Json& S = G("personaSetup");
        std::vector<FlowPersona> personas;
        for (const auto& p : IN("personas")) personas.push_back(persona_from(p));
        Warns w;
        const Json responses[3] = {Json::array({Json{{"ok", true}, {"text", "前置き [{\"personaId\":\"p1\",\"valueAxis\":\" 体験 \",\"coreClaims\":[\" a \",\"\", \"b\", \"c\"]},{\"personaId\":\"p2\",\"valueAxis\":\"\",\"coreClaims\":[\"x\"]},{\"personaId\":\"p3\"},{\"personaId\":\"p1\",\"valueAxis\":\"後勝ち\"}]"}}}),
                                   Json::array({Json{{"ok", true}, {"text", "[broken"}}}), Json::array({Json{{"ok", false}, {"error", "down"}}})};
        for (int i = 0; i < 3; ++i) {
            FakeLLM llm(responses[i]);
            SetupSessionPersonasArgs a;
            a.theme = "T";
            a.sessionId = std::vector<std::string>{"PS", "PS2", "PS3"}[static_cast<std::size_t>(i)];
            a.personas = i == 0 ? personas : std::vector<FlowPersona>{personas[0]};
            if (i == 0) a.model = "m";
            a.warn = w.fn();
            W2G_SAME(list_json(setup_session_personas(llm, a)), S["results"][static_cast<std::size_t>(i)]);
            W2G_SAME(llm.calls, S["calls"][static_cast<std::size_t>(i)]);
        }
        CHECK_EQ(w.list.size(), std::size_t(2));
        CHECK(str::starts_with(w.list[0], "ペルソナ価値軸生成 JSON パース失敗: "));
        CHECK_EQ(w.list[1], S["warns"][1].get<std::string>());
        auto row = db::get_flow_db()->get("SELECT value_axis, core_claims_json, possession_name FROM flow_session_persona WHERE session_id='PS' AND id='p3'");
        CHECK(row.has_value());
        CHECK_EQ(row->str("possession_name"), std::string("論者#abc"));
    }
    {
        const Json& R = G("roundSummary");
        Warns w;
        const Json resp[3] = {Json::array({Json{{"ok", true}, {"text", "  まとめ文  "}}, Json{{"ok", true}, {"text", "{\"found\": true, \"summary\": \"止揚X\"}"}}}),
                              Json::array({Json{{"ok", false}, {"error", "e1"}}, Json{{"ok", true}, {"text", "not json"}}}),
                              Json::array({Json{{"ok", true}, {"text", "   "}}})};
        for (int i = 0; i < 3; ++i) {
            FakeLLM llm(resp[i]);
            RoundSummaryInput in;
            in.theme = "T";
            in.sessionId = "RS";
            in.paperId = "PP";
            in.round = i + 1;
            in.utterances = i < 2 ? std::vector<NamedUtterance>{{"A", "a"}, {"B", "b"}} : std::vector<NamedUtterance>{{"A", "a"}};
            if (i == 0) in.stockedAufhebung = {"前の止揚"};
            in.warn = w.fn();
            const auto out = summarize_round(llm, in);
            W2G_SAME(Json({{"summary", out.summary}, {"aufhebung", out.aufhebung}, {"newAufhebung", out.newAufhebung}}), R["results"][static_cast<std::size_t>(i)]);
            W2G_SAME(llm.calls, R["calls"][static_cast<std::size_t>(i)]);
        }
        W2G_SAME(w.json(), R["warns"]);
        CHECK_EQ(db::get_flow_db()->all("SELECT id FROM discussion_paper_round WHERE paper_id='PP'").size(), std::size_t(3));
        // 冪等: 同じラウンドをもう一度まとめても行は増えない
        FakeLLM again(Json::array({Json{{"ok", true}, {"text", "再"}}}));
        summarize_round(again, {"T", "RS", "PP", 1, {{"A", "a"}}, {}, w.fn(), "discussion"});
        CHECK_EQ(db::get_flow_db()->all("SELECT id FROM discussion_paper_round WHERE paper_id='PP'").size(), std::size_t(3));
        auto row = db::get_flow_db()->get("SELECT summary FROM discussion_paper_round WHERE paper_id='PP' AND round=1");
        CHECK_EQ(row->str("summary"), std::string("再"));
    }
    {
        const Json& F = G("refine");
        Warns w;
        const Json resp[3] = {Json{{"ok", true}, {"text", "  # 議題\n改訂  "}}, Json{{"ok", true}, {"text", "議題なし"}}, Json{{"ok", false}, {"error", "e"}}};
        for (int i = 0; i < 3; ++i) {
            FakeLLM llm(Json::array({resp[i]}));
            RefinePaperBriefInput in;
            in.baseBodyMd = i == 0 ? "  # 議題\nX " : "";
            in.theme = "T";
            in.rounds = rounds_from(G("progress")[0]["rounds"]);
            in.conclusion = " 結論 ";
            in.sessionId = "RF";
            in.model = "m";
            in.warn = w.fn();
            W2G_SAME(opt_json(refine_paper_brief(llm, in)), F["results"][static_cast<std::size_t>(i)]["result"]);
            W2G_SAME(llm.calls, F["results"][static_cast<std::size_t>(i)]["calls"]);
        }
        W2G_SAME(w.json(), F["warns"]);
        // コストログが 1 呼び出し 1 行
        auto row = db::get_flow_db()->get("SELECT COUNT(*) AS n FROM llm_call_log WHERE session_id='RF' AND location='paper-refine'");
        CHECK_EQ(row->i64("n"), 3);
    }
    {
        const Json& Y = G("synthesisLoop");
        const auto posA = w2g::position_from(IN("posA"));
        const auto posB = w2g::position_from(IN("posB"));
        const auto t0 = w2g::tension_from(IN("tensions")[0]);
        Warns w;
        auto ok = [](const char* t) { return Json{{"ok", true}, {"text", t}}; };
        FakeLLM gen(Json::array({ok("{\"text\":\"案1\",\"preserves\":[\"pro-G1\"],\"negates\":[],\"elevates\":\"両方\"}"),
                                 ok("{\"text\":\"案2\",\"preserves\":[\"pro-G1\",\"pro-G3\",\"con-G1\"],\"negates\":[],\"elevates\":\"新しい上位目標を導入して条件で分割する案\"}")}));
        FakeLLM gate(Json::array({ok("aufheben")}));
        FakeLLM rat(Json::array({ok("{\"verdict\":\"reject\",\"preservedIds\":[],\"missing\":[\"pro-G2\"]}"), ok("{\"verdict\":\"accept\",\"preservedIds\":[\"con-G1\"]}"),
                                 ok("{\"verdict\":\"accept\",\"preservedIds\":[\"pro-G1\",\"pro-G3\"]}"), ok("{\"verdict\":\"accept\",\"preservedIds\":[\"con-G1\"]}")}));
        SynthesisLoopArgs a;
        a.issueTitle = "I";
        a.tension = t0;
        a.positionA = posA;
        a.positionB = posB;
        a.personaNames = names_map();
        a.paperSystem = "SYS";
        a.genLlm = &gen;
        a.gateLlm = &gate;
        a.ratifyLlm = &rat;
        a.mainModel = "main";
        a.judgeModel = "judge";
        a.warn = w.fn();
        const auto ra = run_synthesis_loop(a);
        W2G_SAME(Json({{"record", synthesis_record_json(ra.record)}, {"tensionStatus", ra.tensionStatus}}), Y["results"][0]);
        FakeLLM gen2(Json::array({Json{{"ok", false}, {"error", "e"}}}));
        FakeLLM gate2(Json::array({ok("x")}));
        FakeLLM rat2(Json::array({ok("bad")}));
        SynthesisLoopArgs b = a;
        b.personaNames = {};
        b.genLlm = &gen2;
        b.gateLlm = &gate2;
        b.ratifyLlm = &rat2;
        b.mainModel.reset();
        b.judgeModel.reset();
        const auto rb = run_synthesis_loop(b);
        W2G_SAME(Json({{"record", synthesis_record_json(rb.record)}, {"tensionStatus", rb.tensionStatus}}), Y["results"][1]);
        W2G_SAME(Json::array({gen.calls, gate.calls, rat.calls, gen2.calls, gate2.calls, rat2.calls}), Y["calls"]);
        W2G_SAME(w.json(), Y["warns"]);
        // 修正ループは 1 行を上書きする
        // list_syntheses は TS と同じく created_at (ms)・id (UUID) の順。同じ ms に書いた 2 行の順は決まらないので revision で並べて見る。
        auto syn = list_syntheses(t0.id);
        std::sort(syn.begin(), syn.end(), [](const auto& x, const auto& y) { return x.revision < y.revision; });
        CHECK_EQ(syn.size(), std::size_t(2));
        CHECK_EQ(syn[0].revision, 1);
        CHECK_EQ(syn[1].revision, 2);
        CHECK_EQ(syn[1].status, std::string("rejected"));
    }
}

int main() { return di::test::run_all(); }
