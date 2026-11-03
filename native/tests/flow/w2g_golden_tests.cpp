// w2-G: Node 実装の入出力 (native/tests/fixtures/flow/w2g_golden.json) と C++ の出力が同じであることを確かめる。
// 生成: npx tsx native/tests/golden/w2-G-flow.gen.ts
#include "flow/w2g_support.hpp"

#include <map>
#include <cmath>
#include <set>

#include "base/strings.hpp"
#include "flow/argument_graph.hpp"
#include "flow/debatability.hpp"
#include "flow/dialectic/exchange.hpp"
#include "flow/dialectic/exchange_prompt.hpp"
#include "flow/dialectic/goal.hpp"
#include "flow/dialectic/issue_voices.hpp"
#include "flow/dialectic/issues.hpp"
#include "flow/dialectic/outcome.hpp"
#include "flow/dialectic/position.hpp"
#include "flow/dialectic/position_stance.hpp"
#include "flow/dialectic/rule_check.hpp"
#include "flow/dialectic/scheduler.hpp"
#include "flow/dialectic/synthesis.hpp"
#include "flow/dialectic/tension.hpp"
#include "flow/dialectic/turn_prompt.hpp"
#include "flow/discussion_paper.hpp"
#include "flow/effect_predict.hpp"
#include "flow/improvement_score.hpp"
#include "flow/mechanic_extract.hpp"
#include "flow/paper_markdown.hpp"
#include "flow/paper_refine.hpp"
#include "flow/paper_reuse.hpp"
#include "flow/paper_voices.hpp"
#include "flow/persona_setup.hpp"
#include "flow/proxy/position_attribution.hpp"
#include "flow/round_summary.hpp"
#include "flow/spec_analyze.hpp"
#include "flow/user_voices/mix.hpp"
#include "flow/voice_cache.hpp"
#include "flow/vote.hpp"

using namespace di;
using namespace di::flow;
using w2g::FakeLLM;
using w2g::golden;
using w2g::Warns;

namespace {

const Json& G(const char* key) { return golden().at(key); }

std::vector<MechanicSummary> mechs_from(const Json& a) {
    std::vector<MechanicSummary> out;
    for (const auto& m : a) out.push_back(mechanic_summary_from_json(m));
    return out;
}

Json mechs_json(const std::vector<MechanicSummary>& v) {
    Json a = Json::array();
    for (const auto& m : v) a.push_back(to_json(m));
    return a;
}

std::vector<RoundSummary> rounds_from(const Json& a) {
    std::vector<RoundSummary> out;
    for (const auto& r : a) out.push_back({r["round"].get<int>(), r["summary"], w2g::strings(r["aufhebung"])});
    return out;
}

FlowPersona persona_from(const Json& j) { return flow_persona_from_json(j); }

Json opt_json(const std::optional<std::string>& v) { return v ? Json(*v) : Json(nullptr); }

template <class T>
Json list_json(const std::vector<T>& v) {
    Json a = Json::array();
    for (const auto& x : v) a.push_back(to_json(x));
    return a;
}

/** 応答例から呼び出し・警告・結果を比べる共通部。 */
struct LlmCase {
    const Json& c;
    FakeLLM llm;
    Warns warns;
    explicit LlmCase(const Json& cs) : c(cs), llm(cs["responses"]) {}
    void check(const Json& result, const char* label) {
        if (!w2g::same(llm.calls, c["calls"]))
            throw test::Failure(std::string(label) + " calls\n  actual:   " + llm.calls.dump() + "\n  expected: " + c["calls"].dump());
        if (!w2g::same(warns.json(), c["warns"]))
            throw test::Failure(std::string(label) + " warns\n  actual:   " + warns.json().dump() + "\n  expected: " + c["warns"].dump());
        if (!w2g::same(result, c["result"]))
            throw test::Failure(std::string(label) + " result\n  actual:   " + result.dump() + "\n  expected: " + c["result"].dump());
    }
};

} // namespace

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

TEST_CASE("improvement_score: 射影スコアと勝者") {
    const Json& g = G("improvement");
    std::vector<double> gap = g["gap"].get<std::vector<double>>();
    auto mv = [](int k) {
        std::vector<double> v;
        for (int i = 0; i < 20; ++i) v.push_back(std::sin(i + k));
        return v;
    };
    std::vector<MovementCandidate> cands{{"a", mv(1), "effect-predict", std::vector<EffectChange>{{"asp.fun", 0.1, {}}}},
                                         {"b", mv(2), "lexicon-fallback", {}},
                                         {"c", mv(1), "effect-predict", {}}};
    const auto scored = score_opinions(cands, gap);
    // 入力の sin は Node と libm で最後の桁が違いうるので、スコアは誤差つきで比べる。
    for (std::size_t i = 0; i < scored.size(); ++i) {
        Json actual = to_json(scored[i]);
        Json expected = g["scored"][i];
        CHECK(std::abs(actual["score"].get<double>() - expected["score"].get<double>()) < 1e-12);
        actual.erase("score");
        expected.erase("score");
        W2G_SAME(actual, expected);
    }
    W2G_SAME(opt_json(pick_winner(scored)), g["winner"]);
    CHECK(!pick_winner({}).has_value());
    CHECK(std::abs(score_opinion(mv(3), mv(4), gap) - g["opinion"].get<double>()) < 1e-12);
}

TEST_CASE("mechanic_extract: enrichMechanics") {
    for (const auto& c : G("enrich")) {
        LlmCase lc(c);
        EnrichMechanicsArgs a;
        a.theme = "テーマ";
        a.existing = mechs_from(c["existing"]);
        a.voices.push_back({std::string(170 * 3, '\0'), std::string("s")});
        a.voices[0].content.clear();
        for (int i = 0; i < 170; ++i) a.voices[0].content += "い";
        for (int i = 0; i < 30; ++i) a.voices.push_back({"声" + std::to_string(i), std::nullopt});
        a.target = c["target"];
        a.model = "mm";
        a.warn = lc.warns.fn();
        lc.check(mechs_json(enrich_mechanics(lc.llm, a)), "enrich");
    }
}

TEST_CASE("spec_analyze: チャンク分割と抽出") {
    std::string long_spec;
    for (int i = 0; i < 12000; ++i) long_spec += "仕様😀";
    for (const auto& c : G("spec")) {
        LlmCase lc(c);
        AnalyzeSpecMechanicsArgs a;
        a.theme = "T";
        a.specText = c["specText"] == "LONG" ? long_spec : c["specText"].get<std::string>();
        if (c.contains("maxMechanics")) a.maxMechanics = c["maxMechanics"];
        a.warn = lc.warns.fn();
        const auto r = analyze_spec_mechanics(lc.llm, a);
        for (auto& call : lc.llm.calls) {
            const std::string prompt = call["prompt"];
            if (str::js_length(prompt) > 5000) call["prompt"] = "LEN:" + std::to_string(str::js_length(prompt));
        }
        lc.check(list_json(r), "spec");
    }
}

TEST_CASE("vote: parseVoteId / pickVoteWinner") {
    for (const auto& c : G("voteParse")) {
        const auto id = parse_vote_id(c["text"], w2g::strings(c["valid"]), w2g::strings(c["presented"]));
        W2G_SAME(opt_json(id), c["id"]);
    }
    for (const auto& c : G("voteWinner")) {
        std::vector<std::pair<std::string, double>> tally;
        for (auto it = c["tally"].begin(); it != c["tally"].end(); ++it) tally.emplace_back(it.key(), it.value().get<double>());
        const double r = c["r"];
        const auto w = pick_vote_winner(tally, [r] { return r; });
        W2G_SAME(opt_json(w.winner), c["result"]["winner"]);
        CHECK_EQ(w.maxVotes, c["result"]["maxVotes"].get<double>());
    }
}

TEST_CASE("voice_cache / mix / 保存した声の検索") {
    const auto& keys = G("voiceKeys");
    (void)keys;
    std::vector<std::string> seen_terms;
    auto cache = create_voice_cache([&](const std::vector<std::string>& terms, int limit) {
        return std::vector<ContextVoice>{{str::join(terms, "+") + std::to_string(limit), "s", std::nullopt}};
    });
    const Json& e = G("cache");
    W2G_SAME(w2g::voices_json(cache->lookup({"A", "b"}, 3)), e[0]);
    W2G_SAME(w2g::voices_json(cache->lookup({"b ", "a"}, 3)), e[1]);
    W2G_SAME(w2g::voices_json(cache->lookup({"a"}, 3)), e[2]);
    CHECK_EQ(cache->misses(), e[3].get<int>());
    CHECK_EQ(cache->hits(), e[4].get<int>());
    // JS の sort は UTF-16 順 (😀 は U+FF5A ｚ より前)。
    CHECK(voice_cache_key({"ｚ", "😀"}, 1) == voice_cache_key({"😀", "ｚ"}, 1));
    CHECK(voice_cache_key({"ｚ", "😀"}, 1) == std::string("1") + '\0' + "😀ｚ");

    const std::vector<ContextVoice> store{{"ガチャが楽しい", "s", {}}, {"周回がつらい", "s", {}}, {"ガチャと周回", "s2", "u"}};
    for (const auto& c : G("storedLookup"))
        W2G_SAME(w2g::voices_json(stored_voice_lookup(store)(w2g::strings(c["terms"]), c["limit"].get<int>())), c["result"]);
    auto mixed = user_voices::make_mixed_voice_lookup([&](const std::vector<std::string>&, int) {
        return std::vector<std::vector<ContextVoice>>{store, {store[1], {" 周回がつらい ", "x", {}}, {"別", "y", {}}}, {}};
    });
    W2G_SAME(w2g::voices_json(mixed({"x"}, 4)), G("mix"));
    CHECK(mixed({"x"}, 0).empty());
    const auto ev = user_voices::mix_evenly<int>({{1, 2, 3}, {1, 4}, {5}}, 10, [](int v) { return std::to_string(v); });
    W2G_SAME(Json(ev), G("mixEvenly"));
}

TEST_CASE("paper_reuse: 番号の読み取り") {
    for (const auto& c : G("reuseNo")) {
        const auto n = parse_reuse_paper_no(c[0].get<std::string>());
        W2G_SAME(n ? Json(*n) : Json(nullptr), c[1]);
    }
    CHECK(!parse_reuse_paper_no(std::nullopt).has_value());
}

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

namespace {

using namespace di::flow::dialectic;

const Json& IN(const char* key) { return G("inputs").at(key); }

Json position_json(const PositionRecord& p) {
    Json o = Json::object();
    o["id"] = p.id;
    o["issueId"] = p.issueId;
    o["personaId"] = p.personaId;
    o["stance"] = p.stance;
    o["claim"] = p.claim;
    o["qualifier"] = opt_json(p.qualifier);
    o["grounds"] = list_json(p.grounds);
    o["values"] = p.values;
    return o;
}

Json node_json(const ArgumentNode& n) {
    Json o = Json::object();
    o["utteranceId"] = n.utteranceId;
    o["side"] = n.side;
    o["personaId"] = n.personaId;
    o["groundId"] = opt_json(n.groundId);
    o["data"] = n.data;
    o["text"] = n.text;
    if (n.warrant) o["warrant"] = *n.warrant;
    if (n.kind) o["kind"] = *n.kind;
    o["id"] = n.id;
    o["parentId"] = opt_json(n.parentId);
    o["depth"] = n.depth;
    o["status"] = n.status;
    return o;
}

Json nodes_json(const std::vector<ArgumentNode>& v) {
    Json a = Json::array();
    for (const auto& n : v) a.push_back(node_json(n));
    return a;
}

Json synthesis_record_json(const SynthesisRecord& s) {
    Json o = Json::object();
    o["tensionId"] = s.tensionId;
    o["preserves"] = s.preserves;
    o["negates"] = s.negates;
    o["elevates"] = opt_json(s.elevates);
    o["kind"] = s.kind;
    o["text"] = s.text;
    o["ratification"] = list_json(s.ratification);
    o["status"] = s.status;
    o["revision"] = s.revision;
    o["id"] = "<id>";
    return o;
}

std::map<std::string, std::string> names_map() {
    std::map<std::string, std::string> m;
    for (auto it = IN("names").begin(); it != IN("names").end(); ++it) m[it.key()] = it.value();
    return m;
}

FlowPersona P(int i) { return persona_from(IN("personas")[static_cast<std::size_t>(i)]); }

} // namespace

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
