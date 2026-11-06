// w4p_flow のテスト (1/4)。準備は w4p_flow_support.hpp。
#include "w4p_flow_support.hpp"

using namespace w4p_flow_support;

// ─── golden: 進行 ─────────────────────────────────────────────────

TEST_CASE("golden: dialectic / dispatch / 改善 / 壁打ち / 未来予測を Node と同じ応答で最後まで回す") {
    for (const auto& sc : golden()["runs"]) {
        try {
            run_golden(sc);
        } catch (const std::exception& e) {
            throw test::Failure("[" + sc["name"].get<std::string>() + "] " + e.what());
        }
    }
    set_env(Json::object());
    foresight::reset_foresight_status_for_test();
}

TEST_CASE("golden の中身: dialectic-driver.test.ts / proxy-debate.test.ts の観点") {
    // シナリオ 1: 4 表整合・計測型収束・止揚の批准
    {
        const Json db = golden_run("dlx-basic")["db"];
        CHECK_EQ(db["flow_issue"].size(), 3u);
        CHECK_EQ(db["flow_issue"][0]["status"], Json("concluded"));
        CHECK_EQ(db["flow_issue"][1]["status"], Json("open"));
        CHECK_EQ(db["flow_tension"][0]["status"], Json("synthesized"));
        CHECK_EQ(db["flow_synthesis"][0]["kind"], Json("aufheben"));
        CHECK_EQ(db["flow_synthesis"][0]["status"], Json("ratified"));
        int rebuts = 0, concedes = 0, claims = 0;
        for (const auto& u : db["flow_utterance"]) {
            if (u["act"] == "rebut") ++rebuts;
            if (u["act"] == "concede") ++concedes;
            if (u["act"] == "claim") ++claims;
        }
        CHECK_EQ(claims, 6);
        CHECK_EQ(rebuts, 8);
        CHECK_EQ(concedes, 4);
    }
    // 事実対立 (証拠なし) は unresolved_fact で結論に明記
    CHECK(str::contains(golden_run("dlx-fact-unresolved")["result"]["conclusion"].get<std::string>(), "未解決の事実問題"));
    // 批准 reject → 修正 2 回で agreed_disagree
    CHECK_EQ(golden_run("dlx-reject")["db"]["flow_synthesis"][0]["revision"], Json(2));
    CHECK_EQ(golden_run("dlx-reject")["db"]["flow_tension"][0]["status"], Json("agreed_disagree"));
    // 人が指定した論点は打ち切らない
    CHECK_EQ(golden_run("dlx-human-issues")["result"]["rounds"], Json(3));
    // 代理: 対立しない論点は定立を作らない・束に無い根拠の反論は問いに格下げ
    {
        const Json db = golden_run("proxy-e2e")["db"];
        CHECK_EQ(db["flow_position"].size(), 2u);
        bool rebut = false, question = false, notice = false;
        for (const auto& u : db["flow_utterance"]) {
            if (u["act"] == "rebut") rebut = true;
            if (u["act"] == "question") question = true;
            if (str::contains(u["text"].get<std::string>(), "AI の代理")) notice = true;
        }
        CHECK(!rebut);
        CHECK(question);
        CHECK(notice);
    }
    // dispatch の後段: 未来予測が付く (一部失敗)
    CHECK(golden_run("dispatch-dialectic-foresight")["result"]["result"].contains("foresight"));
    // 機密: 外部の声の検索を一度も呼ばない
    CHECK_EQ(golden_run("dispatch-improvement-confidential")["lookups"].size(), 0u);
    // 改善: 効果予測の大きい発話B が世論 / fallback は lexicon-fallback
    const Json scores = golden_run("improvement-fallback")["db"]["improvement_score"];
    for (const auto& s : scores) CHECK_EQ(s["method"], Json("lexicon-fallback"));
}

// ─── golden: 純関数 ───────────────────────────────────────────────

TEST_CASE("golden: vote-display (scores は票数表記にしない)") {
    for (const auto& c : golden()["voteDisplay"]) {
        VoteDisplay e;
        e.kind = c["event"]["kind"];
        for (const auto& [k, v] : c["event"]["tally"].items()) e.tally.emplace_back(k, v.get<double>());
        if (!c["event"]["winner"].is_null()) e.winner = c["event"]["winner"].get<std::string>();
        W3L_SAME(Json(build_vote_ranking_lines(e, [](const std::string& id) { return "名-" + id; })), c["lines"]);
        CHECK_EQ(has_visible_vote_result(e), c["visible"].get<bool>());
        CHECK_EQ(should_stack_vote_reactions(e.kind), c["stack"].get<bool>());
        CHECK_EQ(vote_notice_title(3, e.kind), c["title"].get<std::string>());
    }
}

TEST_CASE("golden: dialectic 決着表 (settlement_to_round_summary / collect_unresolved_facts)") {
    std::map<std::string, std::string> names;
    for (const auto& [k, v] : golden()["settlementNames"].items()) names[k] = v;
    std::vector<dialectic::IssueSettlement> all;
    for (const auto& c : golden()["settlements"]) {
        const Json& s = c["settlement"];
        dialectic::IssueSettlement st;
        st.issue.id = s["issue"]["id"];
        st.issue.title = s["issue"]["title"];
        st.issue.ordinal = s["issue"]["ordinal"];
        for (const auto& p : s["positions"]) {
            dialectic::PositionRecord pr;
            pr.id = p["id"];
            pr.personaId = p["personaId"];
            pr.stance = p["stance"];
            pr.claim = p["claim"];
            st.positions.push_back(pr);
        }
        for (const auto& t : s["tensions"]) {
            dialectic::TensionRecord tr;
            tr.id = t["id"];
            tr.status = t["status"];
            if (!t["resolutionNote"].is_null()) tr.resolutionNote = t["resolutionNote"].get<std::string>();
            st.tensions.push_back(tr);
        }
        for (const auto& x : s["syntheses"]) {
            dialectic::SynthesisRecord sr;
            sr.tensionId = x["tensionId"];
            sr.text = x["text"];
            if (!x["elevates"].is_null()) sr.elevates = x["elevates"].get<std::string>();
            st.syntheses.push_back(sr);
        }
        if (!s["refutation"].is_null()) st.refutation = s["refutation"].get<std::string>();
        if (s.contains("undecided")) st.undecided = s["undecided"].get<std::string>();
        const std::optional<double> share = c["share"].is_null() ? std::nullopt : std::optional<double>(c["share"].get<double>());
        W3L_SAME(to_json(dialectic::settlement_to_round_summary(st, names, share)), c["summary"]);
        all.push_back(st);
    }
    W3L_SAME(Json(dialectic::collect_unresolved_facts(all)), golden()["unresolvedFacts"]);
}

int main() { return di::test::run_all(); }
