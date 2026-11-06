// w4p_flow のテスト (3/4)。準備は w4p_flow_support.hpp。
#include "w4p_flow_support.hpp"

using namespace w4p_flow_support;

TEST_CASE("dispatch 後段: 未来予測 (タグ / 機密・内部 / 未収束 / 例外 / 無効) (foresight.test.ts)") {
    w3l::TempFlowDb tmp;
    set_env(Json::object());
    foresight::reset_foresight_status_for_test();
    std::vector<std::string> prompts;
    auto llm = foresight_llm(prompts);
    std::vector<FlowUtteranceRecord> notified;
    std::optional<std::string> pending_during_run;
    DispatchDeps d;
    d.llm = borrow(llm);
    d.sessionId = "s-dispatch";
    d.onUtterance = [&](const FlowUtteranceRecord& u) { notified.push_back(u); };
    d.listExternalVoices = [](const std::vector<std::string>&, int) { return std::vector<ContextVoice>{{"声その1", "niconico", std::nullopt}}; };
    d.log = [](const std::string&) {};
    d.warn = [](const std::string&) {};
    d.drivers.discussion = [&](persona_engine::LLMClient&, const std::string&, const std::vector<FlowTag>&, const FlowDirectorDeps&) {
        const auto st = foresight::get_foresight_status("s-dispatch");
        pending_during_run = st ? std::optional<std::string>(st->state) : std::nullopt;
        return fake_result("s-dispatch");
    };
    auto in = [](std::vector<std::string> tags) {
        DispatchInput i;
        i.theme = "t";
        i.flow = "議論";
        i.tags = std::move(tags);
        return i;
    };

    const auto untagged = dispatch_flow(in({"開発"}), d);
    CHECK(!untagged.result->foresight);
    CHECK(notified.empty());
    CHECK(!foresight::get_foresight_status("s-dispatch"));

    const auto on = dispatch_flow(in({"未来予測"}), d);
    CHECK_EQ(pending_during_run.value_or(""), std::string("pending"));
    CHECK(on.result->foresight && str::contains(on.result->foresight->forecast, "【3年後】"));
    CHECK_EQ(on.result->utterances.size(), 3u + notified.size());
    CHECK(notified.size() >= 3);
    CHECK_EQ(on.result->conclusion, std::string("結論X"));
    CHECK_EQ(foresight::get_foresight_status("s-dispatch")->state, std::string("done"));
    CHECK(std::any_of(prompts.begin(), prompts.end(), [](const std::string& p) { return str::contains(p, "声その1"); }));
    // 未来予測の発話は flow_utterance に永続される
    {
        auto db = flow::db::get_flow_db();
        CHECK_EQ(db->get("SELECT COUNT(*) AS n FROM flow_utterance WHERE session_id = 's-dispatch'")->i64("n"),
                 static_cast<std::int64_t>(notified.size()));
    }

    // 内部/機密は本議論にも未来予測にも蓄積済みの外部の声を渡さない
    for (const char* deny : {"内部", "機密"}) {
        const std::size_t before = prompts.size();
        bool driver_has_voices = true;
        DispatchDeps gd = d;
        gd.drivers.discussion = [&](persona_engine::LLMClient&, const std::string&, const std::vector<FlowTag>&, const FlowDirectorDeps& fd) {
            driver_has_voices = static_cast<bool>(fd.listExternalVoices);
            return fake_result("s-dispatch");
        };
        const auto gated = dispatch_flow(in({"未来予測", deny}), gd);
        CHECK(!driver_has_voices);
        CHECK(gated.result->foresight.has_value());
        for (std::size_t k = before; k < prompts.size(); ++k) CHECK(!str::contains(prompts[k], "声その1"));
    }

    // 未収束なら行わない
    notified.clear();
    DispatchDeps ud = d;
    ud.drivers.discussion = [](persona_engine::LLMClient&, const std::string&, const std::vector<FlowTag>&, const FlowDirectorDeps&) {
        auto r = fake_result("s-dispatch");
        r.conclusion = "";
        r.concluded = false;
        return r;
    };
    const auto unconcluded = dispatch_flow(in({"未来予測"}), ud);
    CHECK(!unconcluded.result->foresight);
    CHECK(notified.empty());
    CHECK(foresight::get_foresight_status("s-dispatch") == (foresight::ForesightStatus{"skipped", std::string("本議論が収束していない")}));

    // 本議論の例外は状態を失敗にして投げ直す
    DispatchDeps ed = d;
    ed.drivers.discussion = [](persona_engine::LLMClient&, const std::string&, const std::vector<FlowTag>&, const FlowDirectorDeps&) -> FlowDirectorResult {
        throw std::runtime_error("engine down");
    };
    try {
        (void)dispatch_flow(in({"未来予測"}), ed);
        throw test::Failure("should rethrow");
    } catch (const std::runtime_error& e) {
        CHECK_EQ(std::string(e.what()), std::string("engine down"));
    }
    CHECK_EQ(foresight::get_foresight_status("s-dispatch")->state, std::string("failed"));

    // 機能が無効ならタグがあっても付けない
    set_env(Json{{"DISCUTERE_FLOW_FORESIGHT_ENABLED", "false"}});
    foresight::reset_foresight_status_for_test();
    notified.clear();
    const auto off = dispatch_flow(in({"未来予測"}), d);
    CHECK(!off.result->foresight);
    CHECK(notified.empty());
    CHECK(!foresight::get_foresight_status("s-dispatch"));
    set_env(Json::object());
    foresight::reset_foresight_status_for_test();
}

TEST_CASE("未来予測フェーズ: ペーパー追記の失敗も一部失敗に数える / commit 必須") {
    w3l::TempFlowDb tmp;
    set_env(Json::object());
    std::vector<std::string> prompts;
    auto llm = foresight_llm(prompts);
    foresight::ForesightPhaseArgs pa;
    pa.theme = "t";
    pa.sessionId = "s";
    pa.paperId = "p";
    pa.flow = "discussion";
    pa.utterances = fake_result("s").utterances;
    pa.conclusion = "c";
    pa.turns = 2;
    pa.commit = [](const FlowUtteranceRecord&) {};
    pa.paperBody = foresight::PaperBodyAccess{[] { return std::optional<std::string>("# 結論\nY"); },
                                              [](const std::string&) { throw std::runtime_error("db locked"); }};
    const auto r = foresight::run_foresight_phase(llm, pa);
    CHECK_EQ(r->status, std::string("degraded"));
    CHECK_EQ(r->failures, 1);
    pa.commit = nullptr;
    CHECK_THROWS(foresight::run_foresight_phase(llm, pa));
    // 話者 1 人なら行わない
    pa.commit = [](const FlowUtteranceRecord&) {};
    pa.utterances.resize(2);
    CHECK(!foresight::run_foresight_phase(llm, pa));
}

// ─── 進行状態・並行 ─────────────────────────────────────────────

TEST_CASE("進行状態: dialectic を別スレッドから読み、request_stop で FlowStopped (発話は残り結論は作らない)") {
    w3l::TempFlowDb tmp;
    set_env(golden_run("dlx-basic")["env"]);
    w3l::Normalizer norm;
    w3l::ReplayLLM llm(golden_run("dlx-basic")["calls"], norm);
    auto state = std::make_shared<FlowRunState>();
    std::atomic<int> seen{0};
    FlowRunOptions o;
    o.sessionId = "dlx-stop";
    o.rng = w3l::lcg(1234);
    o.gamesDir = tmp.dir + "/games";
    o.log = [](const std::string&) {};
    o.warn = [](const std::string&) {};
    o.state = state;
    o.onUtterance = [&](const FlowUtteranceRecord&) {
        if (++seen == 5) {
            std::thread([state] { state->request_stop(); }).join();
        }
    };
    bool stopped = false;
    try {
        (void)dialectic::run_dialectic_flow(llm, "ガチャ緩和の是非", {}, o);
    } catch (const FlowStopped&) {
        stopped = true;
    }
    CHECK(stopped);
    const auto snap = state->snapshot();
    CHECK_EQ(snap.phase, std::string("stopped"));
    CHECK_EQ(snap.utterances, 5);
    CHECK_EQ(snap.round, 1);
    CHECK_EQ(snap.rounds, 3);
    auto db = flow::db::get_flow_db();
    CHECK_EQ(db->get("SELECT COUNT(*) AS n FROM flow_utterance WHERE session_id = 'dlx-stop'")->i64("n"), 5);
    CHECK_EQ(db->get("SELECT COUNT(*) AS n FROM flow_conclusion WHERE session_id = 'dlx-stop'")->i64("n"), 0);
    set_env(Json::object());
}

TEST_CASE("壁打ち: 同じセッションへの並行の submit_user は直列化され、暴走ガードを超えない / start 前は拒否") {
    w3l::TempFlowDb tmp;
    set_env(Json{{"DISCUTERE_FLOW_PERSONA_COUNT", "2"}, {"DISCUTERE_FLOW_SPARRING_MAX_TURNS", "5"}});
    std::atomic<int> llm_calls{0};
    w3l::FnLLM llm([&](const persona_engine::LLMInvokeArgs& a) {
        ++llm_calls;
        if (str::contains(a.prompt, "found=false")) return persona_engine::LLMResult::success(R"({"found":false,"summary":""})");
        return persona_engine::LLMResult::success("応答");
    });
    SparringDeps d;
    d.llm = borrow(llm);
    d.rng = w3l::lcg(3);
    d.gamesDir = tmp.dir + "/no-games";
    d.log = [](const std::string&) {};
    d.warn = [](const std::string&) {};
    auto session = std::make_shared<SparringSession>("テーマ", std::vector<FlowTag>{}, d);
    CHECK_THROWS(session->submit_user("早すぎる"));
    session->start();
    std::atomic<int> ended{0}, responses{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([&, t] {
            for (int k = 0; k < 5; ++k) {
                const auto r = session->submit_user("発言 " + std::to_string(t) + "-" + std::to_string(k));
                if (r.kind == "ended") ++ended;
                if (r.kind == "responses") responses += static_cast<int>(r.utterances.size());
            }
        });
    for (auto& th : threads) th.join();
    CHECK(session->is_ended());
    CHECK_EQ(responses.load(), 5);  // sparringMaxTurns
    CHECK(ended.load() >= 1);
    auto db = flow::db::get_flow_db();
    const auto ai = db->get("SELECT COUNT(*) AS n FROM flow_utterance WHERE session_id = ? AND role != 'user'", {session->session_id()})->i64("n");
    CHECK_EQ(ai, 5);
    CHECK_EQ(db->get("SELECT COUNT(*) AS n FROM vote WHERE session_id = ?", {session->session_id()})->i64("n"), 0);
    set_env(Json::object());
}

int main() { return di::test::run_all(); }
