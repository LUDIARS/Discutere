// w2g_golden のテスト (3/4)。準備は w2g_golden_support.hpp。
#include "w2g_golden_support.hpp"

using namespace w2g_golden_support;

TEST_CASE("debatability: 証拠バランスと評価") {
    std::vector<DebatabilityVoice> voices{{"とても楽しい最高", "steam"}, {"つまらない最悪", "steam"}, {"普通", " "}, {"楽しい", std::nullopt}};
    W2G_SAME(to_json(compute_evidence_balance(voices)), G("evidence"));
    W2G_SAME(to_json(compute_evidence_balance({})), G("evidenceEmpty"));
    for (const auto& c : G("debatability")) {
        LlmCase lc(c);
        AssessDebatabilityArgs a;
        a.theme = "議題";
        a.paperMd = "# 議題\nX";
        a.voices = voices;
        a.minArmableIssues = c["min"];
        a.model = "jm";
        a.toVector = [](const std::string& t) {
            return std::vector<double>{str::contains(t, "楽しい") ? 0.9 : str::contains(t, "最悪") ? 0.1 : 0.5};
        };
        a.warn = lc.warns.fn();
        const auto r = assess_debatability(lc.llm, a);
        lc.check(to_json(r), "debatability");
        W2G_SAME(Json(annotated_issues(r)), c["annotated"]);
    }
}

TEST_CASE("dialectic: 論破・結論区分・向き・声のブロック") {
    const auto posA = w2g::position_from(IN("posA"));
    const auto posB = w2g::position_from(IN("posB"));
    const auto posAdown = w2g::position_from(IN("posAdown"));
    const auto posBdown = w2g::position_from(IN("posBdown"));
    const std::vector<std::pair<PositionRecord, PositionRecord>> pairs{{posA, posB}, {posAdown, posB}, {posA, posBdown}, {posAdown, posBdown}};
    for (std::size_t i = 0; i < pairs.size(); ++i) {
        const Json& e = G("goal")[i];
        W2G_SAME(Json(standing_ground_ids(pairs[i].first)), e["standingA"]);
        const auto r = detect_refutation(pairs[i].first, pairs[i].second);
        Json refuted = Json::array();
        for (const auto& p : r.refuted) refuted.push_back(position_json(p));
        W2G_SAME(refuted, e["result"]["refuted"]);
        W2G_SAME(opt_json(r.note), e["result"]["note"]);
        W2G_SAME(opt_json(r.undecided), e["result"]["undecided"]);
    }
    std::vector<TensionRecord> tensions;
    for (const auto& t : IN("tensions")) tensions.push_back(w2g::tension_from(t));
    SynthesisRecord synth;
    synth.text = IN("synthRec")["text"];
    const Json& oc = G("outcome");
    for (std::size_t i = 0; i < tensions.size(); ++i) {
        W2G_SAME(opt_json(no_conclusion_reason(tensions[i])), oc["reasons"][i][0]);
        W2G_SAME(opt_json(no_conclusion_reason(tensions[i], &synth)), oc["reasons"][i][1]);
    }
    CHECK_EQ(issue_outcome(std::string("x"), tensions), oc["outcomes"][0].get<std::string>());
    CHECK_EQ(issue_outcome(std::nullopt, tensions), oc["outcomes"][1].get<std::string>());
    CHECK_EQ(issue_outcome(std::nullopt, {tensions[0]}), oc["outcomes"][2].get<std::string>());
    CHECK_EQ(render_outcome_line("aufheben", "詳細"), oc["line"].get<std::string>());

    const auto issue = w2g::issue_from(IN("issue"));
    const Json& st = G("stance");
    CHECK_EQ(build_stance_prompt(issue, "主張", std::nullopt), st["prompt"][0].get<std::string>());
    CHECK_EQ(build_stance_prompt(issue, "主張", std::string("限定")), st["prompt"][1].get<std::string>());
    const std::vector<std::string> answers{"Yes", "no, yes", "UNCLEAR yes", "nothing", "none"};
    for (std::size_t i = 0; i < answers.size(); ++i) CHECK_EQ(parse_stance_answer(answers[i]), st["parse"][i].get<std::string>());
    CHECK(is_reversed("pro", "no") == st["reversed"][0].get<bool>());
    CHECK(is_reversed("con", "no") == st["reversed"][1].get<bool>());
    CHECK(is_reversed("pro", "unclear") == st["reversed"][2].get<bool>());
    CHECK_EQ(stance_feedback("pro", "前"), st["feedback"][0].get<std::string>());
    CHECK_EQ(stance_feedback("con", "前"), st["feedback"][1].get<std::string>());

    const auto& iv = G("issueVoices");
    CHECK_EQ(render_issue_voices({}), iv[0].get<std::string>());
    std::string emoji;
    for (int i = 0; i < 100; ++i) emoji += "😀";
    CHECK_EQ(render_issue_voices({{"  a\n\nb  ", "s", {}}, {emoji, "t", {}}}), iv[1].get<std::string>());
}

TEST_CASE("dialectic: 定立・前提ルール・矛盾・止揚のプロンプトとパース") {
    const auto issue = w2g::issue_from(IN("issue"));
    const auto posA = w2g::position_from(IN("posA"));
    const auto posB = w2g::position_from(IN("posB"));
    CHECK_EQ(build_position_prompt({P(2), "con", issue, std::string("VOICES"), std::string("FB")}),
             G("positionPrompt")[0].get<std::string>());
    CHECK_EQ(build_position_prompt({P(0), "pro", issue, std::nullopt, std::nullopt}), G("positionPrompt")[1].get<std::string>());

    const Json& rcj = G("ruleCheck");
    for (const auto& c : rcj["anchors"]) {
        const Json& cl = c["claim"];
        const auto r = check_rebut_anchor({w2g::opt(cl, "attack"), w2g::opt(cl, "groundId"), w2g::opt(cl, "data"), w2g::opt(cl, "warrant")}, posA);
        Json o = Json::object();
        o["ok"] = r.ok;
        if (r.ok) o["ground"] = r.ground ? to_json(*r.ground) : Json(nullptr);
        else o["foul"] = r.foul;
        W2G_SAME(o, c["result"]);
    }
    CHECK_EQ(build_rule_judge_prompt({issue, posA, posB, posA.grounds[0], "data", "反論", "rd", "rw"}), rcj["judgePrompt"][0].get<std::string>());
    CHECK_EQ(build_rule_judge_prompt({issue, posB, std::nullopt, std::nullopt, "qualifier", "反論", "rd", "rw"}),
             rcj["judgePrompt"][1].get<std::string>());
    const std::vector<std::string> verdicts{"OK", "straw_man だけど ok", "topic_shift", "none", "ok dismissal"};
    for (std::size_t i = 0; i < verdicts.size(); ++i) W2G_SAME(opt_json(parse_rule_verdict(verdicts[i])), rcj["verdicts"][i]);
    CHECK_EQ(build_concession_prompt("相手", "返答"), rcj["concessionPrompt"].get<std::string>());
    const std::vector<std::string> fouls{"incomplete", "quote_mismatch", "dismissal"};
    for (std::size_t i = 0; i < fouls.size(); ++i) CHECK_EQ(render_foul_notice("A", fouls[i]), rcj["notices"][i].get<std::string>());

    const Json& tj = G("tensionPure");
    CHECK_EQ(build_classify_prompt(issue, posA, posB), tj["classify"].get<std::string>());
    const std::vector<std::string> types{"Means and values", "TRADEOFF", "x", "facts"};
    for (std::size_t i = 0; i < types.size(); ++i) W2G_SAME(opt_json(parse_tension_type(types[i])), tj["types"][i]);
    for (const auto& f : tj["facts"]) W2G_SAME(opt_json(parse_fact_verdict(f[0].get<std::string>())), f[1]);
    CHECK_EQ(build_fact_resolve_prompt(issue, posA, posB, {{std::string(250, 'x'), "s", {}}}), tj["factPrompt"].get<std::string>());

    const Json& sj = G("synthPure");
    const std::vector<std::string> ttypes{"values", "means", "tradeoff", "facts"};
    for (std::size_t i = 0; i < ttypes.size(); ++i) CHECK_EQ(synthesis_template_for(ttypes[i]), sj["templates"][i].get<std::string>());
    auto t0 = w2g::tension_from(IN("tensions")[0]);
    CHECK_EQ(build_synthesis_prompt({"論点", t0, posA, posB, std::nullopt}), sj["prompt"][0].get<std::string>());
    auto t_means = t0;
    t_means.type = "means";
    CHECK_EQ(build_synthesis_prompt({"論点", t_means, posA, posB, std::vector<std::string>{"pro-G1"}}), sj["prompt"][1].get<std::string>());
    CHECK_EQ(build_ratify_prompt({posA, "A", "統合", {}, std::string("枠")}), sj["ratify"].get<std::string>());
}

TEST_CASE("dialectic: 発話プロンプトとターンのパース") {
    const auto issue = w2g::issue_from(IN("issue"));
    const auto posA = w2g::position_from(IN("posA"));
    const auto posB = w2g::position_from(IN("posB"));
    std::map<std::string, UtteranceDigest> utts;
    for (const auto& u : IN("utterances")) utts[u["id"]] = {u["id"], u["personaName"], u["text"]};
    DialecticTurnPromptArgs a;
    a.persona = P(0);
    a.stance = "pro";
    a.issue = issue;
    a.positions = {posA, posB};
    a.unresolvedRebuttals = {{"u2", "u1", "p3", std::string("p1"), "challenged"}, {"zz", "yy", "p3", std::nullopt, "challenged"}};
    a.utterances = utts;
    a.personaNames = names_map();
    a.allowedActs = {"support", "rebut"};
    a.targetUtteranceId = "u2";
    a.nomination = "どうぞ";
    a.voicesBlock = "VB";
    CHECK_EQ(build_dialectic_turn_prompt(a), G("turnPrompt")[0].get<std::string>());
    DialecticTurnPromptArgs b;
    b.persona = P(1);
    b.stance = "opinion";
    b.issue = issue;
    b.utterances = utts;
    b.personaNames = names_map();
    b.allowedActs = {"claim"};
    CHECK_EQ(build_dialectic_turn_prompt(b), G("turnPrompt")[1].get<std::string>());
    for (const auto& c : G("parseTurn")) {
        Warns w;
        const auto r = parse_turn_response(c["raw"], {{"rebut", "question", "support"}, {"u1", "u2"}, std::string("u2"), w.fn()});
        W2G_SAME(to_json(r), c["result"]);
        W2G_SAME(w.json(), c["warns"]);
    }
    for (const auto& c : G("parseExchange")) {
        Warns w;
        W2G_SAME(to_json(parse_exchange_response(c["raw"], w.fn())), c["result"]);
        W2G_SAME(w.json(), c["warns"]);
    }
}

TEST_CASE("dialectic: 論拠の応酬 (幅優先・三値の決着・根拠状態)") {
    GroundExchange ex({3});
    auto nn = [](const std::string& side, const std::string& persona, const std::string& data,
                 std::optional<std::string> gid = std::nullopt) {
        return NewNode{"u-" + data, side, persona, gid, data, {}, {}, "say " + data};
    };
    const auto o1 = ex.add_opening(nn("pro", "p1", "g1", "pro-G1"));
    const auto o2 = ex.add_opening(nn("pro", "p1", "g2", "pro-G2"));
    const auto o3 = ex.add_opening(nn("con", "p3", "g3", "con-G1"));
    // TS は next() が返したノードを後から書き換えるので、記録は最後の状態で比べる (id を控えて後で引く)。
    std::vector<std::optional<std::string>> seen;
    auto nx = [&] {
        auto n = ex.next();
        seen.push_back(n ? std::optional<std::string>(n->id) : std::nullopt);
    };
    nx();
    const auto r1 = ex.rebut(o1.id, nn("con", "p3", "r1"));
    ex.accept(o2.id);
    nx();
    const auto r2 = ex.rebut(o3.id, nn("pro", "p1", "r2"));
    nx();
    ex.fail(r1.id);
    const auto r3 = ex.rebut(r2.id, nn("con", "p3", "r3"));
    const auto r4 = ex.rebut(r3.id, nn("pro", "p1", "r4"));
    nx();
    const int cut = ex.cut_remaining();
    const bool settled = ex.settled();
    Json log = Json::array();
    for (const auto& id : seen) log.push_back(id ? node_json(*ex.get(*id)) : Json(nullptr));
    log.push_back(cut);
    log.push_back(settled);
    const Json& e = G("exchange");
    W2G_SAME(log, e["log"]);
    W2G_SAME(nodes_json(ex.all()), e["all"]);
    W2G_SAME(nodes_json(ex.path(r4.id)), e["path"]);
    W2G_SAME(nodes_json(ex.children(o3.id)), e["children"]);
    Json verdicts = Json::array();
    for (const auto& n : ex.all()) verdicts.push_back(Json::array({n.id, ex.verdict(n.id)}));
    W2G_SAME(verdicts, e["verdicts"]);
    Json gs = Json::array();
    for (const auto& [id, s] : ex.ground_states()) gs.push_back(Json::array({id, s}));
    W2G_SAME(gs, e["groundStates"]);
    for (int i = 0; i < 2; ++i) {
        const auto c = ex.standing_count(i == 0 ? "pro" : "con");
        W2G_SAME(Json({{"standing", c.standing}, {"total", c.total}}), e["standing"][static_cast<std::size_t>(i)]);
    }
    CHECK_EQ(opposing_side("pro"), e["opposing"].get<std::string>());
    CHECK_THROWS(ex.accept("N999"));

    const auto issue = w2g::issue_from(IN("issue"));
    ExchangePromptArgs pa;
    pa.persona = P(2);
    pa.stance = "con";
    pa.issue = issue;
    pa.positions = {w2g::position_from(IN("posA")), w2g::position_from(IN("posB"))};
    pa.personaNames = names_map();
    pa.path = ex.path(r3.id);
    pa.voicesBlock = "VB";
    CHECK_EQ(build_exchange_prompt(pa), e["prompt"].get<std::string>());
    ExchangePromptArgs pb;
    pb.persona = P(0);
    pb.stance = "opinion";
    pb.issue = issue;
    CHECK_EQ(build_exchange_prompt(pb), e["prompt2"].get<std::string>());
}

TEST_CASE("dialectic: スケジューラと引用の切り出し") {
    ArgumentGraph g;
    g.apply({"c1", "p1", "claim", std::nullopt, 1});
    g.apply({"c2", "p3", "claim", std::nullopt, 2});
    g.apply({"r1", "p3", "rebut", std::string("c1"), 3});
    std::vector<FlowPersona> speakers;
    for (const auto& s : IN("speakers")) speakers.push_back(persona_from(s));
    std::vector<PositionAnchor> anchors;
    for (const auto& a : IN("anchors")) anchors.push_back({a["personaId"], a["stance"], a["utteranceId"]});
    std::map<std::string, int> counts;
    for (auto it = IN("counts").begin(); it != IN("counts").end(); ++it) counts[it.key()] = it.value();
    ArgumentGraph empty;
    const auto d1 = schedule_next_turn({&g, speakers, anchors, counts});
    const auto d2 = schedule_next_turn({&g, {speakers.begin() + 1, speakers.end()}, anchors, counts});
    const auto d3 = schedule_next_turn({&empty, {P(1)}, {}, {}});
    const auto d4 = schedule_next_turn({&g, {}, {}, counts});
    const Json& e = G("scheduler");
    const std::vector<std::optional<ScheduleDecision>> ds{d1, d2, d3, d4};
    for (std::size_t i = 0; i < ds.size(); ++i) W2G_SAME(ds[i] ? to_json(*ds[i]) : Json(nullptr), e["decisions"][i]);
    const UtteranceDigest tgt{IN("tgt")["id"], IN("tgt")["personaName"], IN("tgt")["text"]};
    std::size_t k = 0;
    for (const auto& d : {*d1, *d2, *d3}) {
        CHECK_EQ(nomination_fallback_text(d, "A", tgt), e["fallback"][k].get<std::string>());
        CHECK_EQ(nomination_frame_text(d, "A", tgt), e["frame"][k].get<std::string>());
        CHECK_EQ(nomination_fallback_text(d, "A"), e["fallback"][k + 1].get<std::string>());
        CHECK_EQ(nomination_frame_text(d, "A"), e["frame"][k + 1].get<std::string>());
        k += 2;
    }
    std::string a170, i160, b200, emoji;
    for (int i = 0; i < 170; ++i) a170 += "あ";
    for (int i = 0; i < 160; ++i) i160 += "い";
    for (int i = 0; i < 100; ++i) emoji += "😀";
    const std::vector<std::string> quotes{"短い", a170, "一文目です。" + i160, "a。" + std::string(200, 'b'), " a\n b ", emoji};
    for (std::size_t i = 0; i < quotes.size(); ++i) CHECK_EQ(quote_excerpt(quotes[i]), e["quotes"][i].get<std::string>());
}

int main() { return di::test::run_all(); }
