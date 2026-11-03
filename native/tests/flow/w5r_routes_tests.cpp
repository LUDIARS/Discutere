// w5-R: src/flow/web/routes.ts の移植の検証。
// tests/flow/entrypoints.test.ts の Web ルート部分 (議論一覧・下書き再開・修正提案・status のマーク・削除・合成の声) を
// golden (Node の応答と LLM 呼び出し列) で確かめ、加えて C++ 版の背景実行 (草案の構築・承認・停止・満杯 503) と本文上限の境界を見る。
#include "check.hpp"
#include "w4q_support.hpp"
#include "w5r_support.hpp"

#include <atomic>
#include <condition_variable>
#include <thread>

#include "base/crypto.hpp"
#include "base/strings.hpp"
#include "base/executor.hpp"
#include "flow/db/connection.hpp"
#include "flow/discussion_paper.hpp"
#include "flow/investigate.hpp"
#include "flow/web/page.hpp"
#include "flow/web/routes.hpp"

using namespace di;
using namespace di::flow;
using w4q::body_json;
using w4q::make_request;

namespace {

NewPaper new_paper_from(const Json& j) {
    NewPaper p;
    p.sessionId = j["sessionId"].get<std::string>();
    p.theme = j["theme"].get<std::string>();
    p.tags = j["tags"].get<std::vector<std::string>>();
    for (const auto& m : j["mechanics"]) p.mechanics.push_back(mechanic_summary_from_json(m));
    p.supplement = j["supplement"].get<std::string>();
    p.bodyMd = j["bodyMd"].get<std::string>();
    return p;
}

void run_setup(const Json& setup) {
    auto db = db::get_flow_db();
    for (const auto& s : setup) {
        const auto op = s["op"].get<std::string>();
        if (op == "persistPaper") persist_paper(new_paper_from(s["paper"]), s["flow"].get<std::string>());
        else if (op == "persistDraftPaper") persist_draft_paper(new_paper_from(s["paper"]), s["flow"].get<std::string>());
        else if (op == "sql") db->exec(s["sql"].get<std::string>());
        else if (op == "titleCache") upsert_discussion_title_cache(s["sessionId"].get<std::string>(), s["raw"].get<std::string>(), s["source"].get<std::string>());
        else if (op == "reviewInfo") set_paper_review_info(s["sessionId"].get<std::string>(), s["info"]);
        else throw test::Failure("unknown setup op " + op);
    }
}

http::Response call(const http::Router& r, const std::string& method, const std::string& target, std::optional<std::string> body = std::nullopt) {
    Json headers = Json::object();
    if (body) headers["content-type"] = "application/json";
    auto req = make_request(method, target, headers, "127.0.0.1", body.value_or(""));
    return r.handle(req);
}

struct App {
    w4q::Env env;
    std::shared_ptr<w5r::FakeLLM> llm = std::make_shared<w5r::FakeLLM>();
    w5r::FakeVoices voices;
    http::Router router = web::flow_routes();
    App() {
        web::reset_flow_web_for_test();
        web::FlowWebDeps d;
        d.workspaceId = "ws";
        d.llm = llm;
        d.gamesDir = env.dir.file("no-games");
        d.listExternalVoices = voices.fn();
        // 外部 I/O は偽物 (呼ばれたら失敗)
        d.notionCrawler = [](const std::string&, int, int) -> NotionCrawlResult { throw std::runtime_error("no network"); };
        d.specFetch = [](const http::ClientRequest&) -> http::ClientResponse { throw std::runtime_error("no network"); };
        d.githubToken = [] { return std::optional<std::string>(); };
        web::set_flow_web_deps(std::move(d));
    }
    ~App() {
        web::wait_flow_web_idle_for_test(std::chrono::seconds(60));
        web::reset_flow_web_for_test();
    }
};

} // namespace

TEST_CASE("golden: Web ルートの応答と LLM 呼び出し列が Node と一致") {
    App app;
    const Json fx = w5r::load_fixture("routes.json");
    run_setup(fx["setup"]);
    app.llm->take_calls();
    app.voices.take();
    Json revisions = nullptr;
    Json review_info_after = nullptr;
    for (const auto& rec : fx["requests"]) {
        const auto& r = rec["req"];
        const std::string method = r["method"].get<std::string>();
        if (method == "DELETE" && revisions.is_null()) {
            revisions = Json::array();
            for (const auto& row : db::get_flow_db()->all(
                     "SELECT session_id, rev, body_md, change_summary, origin FROM discussion_paper_revision ORDER BY session_id, rev"))
                revisions.push_back(row.to_json());
            const auto info = get_paper_review_info("draft-sess-1");
            review_info_after = info ? *info : Json(nullptr);
        }
        std::optional<std::string> body;
        if (r.contains("rawBody")) body = r["rawBody"].get<std::string>();
        else if (r.contains("body")) body = json_stringify(r["body"]);
        const auto res = call(app.router, method, r["url"].get<std::string>(), body);
        const std::string what = method + " " + r["url"].get<std::string>() + " " + (body ? body->substr(0, 120) : "");
        CHECK_EQ(res.status, rec["status"].get<int>());
        Json actual = body_json(res);
        Json expected = rec["body"];
        w5r::mask_times(actual);
        w5r::mask_times(expected);
        W5R_CHECK_JSON(actual, expected, what + " body");
        W5R_CHECK_JSON(app.llm->take_calls(), rec["calls"], what + " calls");
        W5R_CHECK_JSON(app.voices.take(), rec["voiceCalls"], what + " voiceCalls");
    }
    Json exp_rev = fx["revisions"];
    w5r::mask_times(exp_rev);
    w5r::mask_times(revisions);
    W5R_CHECK_JSON(revisions, exp_rev, "revisions");
    Json exp_info = fx["reviewInfoAfter"];
    w5r::mask_times(exp_info);
    w5r::mask_times(review_info_after);
    W5R_CHECK_JSON(review_info_after, exp_info, "reviewInfoAfter");

    const auto page = call(app.router, "GET", "/flow");
    CHECK_EQ(page.status, fx["page"]["status"].get<int>());
    CHECK_EQ(page.headers.get("Content-Type").value_or(""), fx["page"]["contentType"].get<std::string>());
    CHECK_EQ(crypto::sha256_hex(page.body), fx["page"]["sha256"].get<std::string>());
}

TEST_CASE("golden: normalizeSyntheticLearningOpinions が Node と一致 (entrypoints.test.ts の観点を含む)") {
    const Json fx = w5r::load_fixture("synthetic.json");
    for (const auto& c : fx) {
        Json out = Json::array();
        for (const auto& o : web::normalize_synthetic_learning_opinions(c["items"], 1000))
            out.push_back(Json{{"content", o.content}, {"postedAt", o.postedAt.value_or(-1)}});
        W5R_CHECK_JSON(out, c["out"], c["items"].dump().substr(0, 200));
    }
    // TS テストの観点: 同じセグメント+論点の重複は除外 / 仮説ラベルと軸を保持 / 異なる層は残す
    const auto first = fx[0]["out"];
    CHECK_EQ(first.size(), std::size_t{5});
    for (const auto& v : first) CHECK(str::starts_with(v["content"].get<std::string>(), "[synthetic] ("));
}

TEST_CASE("境界: 各ルートの本文上限と JSON の上限") {
    App app;
    CHECK_EQ(app.router.max_body_for("POST", "/api/flow/start"), web::kFlowStartMaxBody);
    CHECK_EQ(app.router.max_body_for("POST", "/api/flow/x/paper/approve"), web::kFlowApproveMaxBody);
    CHECK_EQ(app.router.max_body_for("POST", "/api/flow/x/paper/form/apply"), web::kFlowEditMaxBody);
    CHECK_EQ(app.router.max_body_for("POST", "/api/flow/x/paper/block/apply"), web::kFlowEditMaxBody);
    CHECK_EQ(app.router.max_body_for("POST", "/api/flow/x/paper/fix-suggestion/apply"), web::kFlowEditMaxBody);
    for (const char* p : {"/api/flow/x/say", "/api/flow/x/paper/block/review", "/api/flow/x/paper/crawl", "/api/flow/x/paper/revert",
                          "/api/flow/x/paper/debatability/check", "/api/flow/x/paper/mechanics/check", "/api/flow/x/paper/edit"})
        CHECK_EQ(app.router.max_body_for("POST", p), web::kFlowSmallMaxBody);

    // 上限ちょうどの JSON は読む (flow が無いので 議論タイプ必須)、上限 + 1 は読まない (= {} でテーマ必須)
    auto padded = [](std::size_t total) {
        const std::string head = R"({"theme":"t","pad":")";
        const std::string tail = R"("})";
        return head + std::string(total - head.size() - tail.size(), 'x') + tail;
    };
    auto exact = call(app.router, "POST", "/api/flow/start", padded(web::kFlowStartMaxBody));
    CHECK_EQ(exact.status, 400);
    CHECK_EQ(body_json(exact)["error"].get<std::string>(), std::string("議論タイプ (必須) を選択してください"));
    auto over = call(app.router, "POST", "/api/flow/start", padded(web::kFlowStartMaxBody + 1));
    CHECK_EQ(over.status, 400);
    CHECK_EQ(body_json(over)["error"].get<std::string>(), std::string("テーマは必須です"));
    // 壊れた JSON・深すぎる JSON・object 以外は {} 扱い
    for (const std::string& b : {std::string("{"), std::string(200, '[') + std::string(200, ']'), std::string("null"), std::string("[1,2]"),
                                 std::string("\"s\"")}) {
        auto r = call(app.router, "POST", "/api/flow/start", b);
        CHECK_EQ(r.status, 400);
        CHECK_EQ(body_json(r)["error"].get<std::string>(), std::string("テーマは必須です"));
    }
    // 修正提案の追記: 8000 文字 (UTF-16) ちょうどは通る / 8001 は 400 (golden に 8001 あり)
    persist_draft_paper(NewPaper{"lim-sess", "議題", {}, {}, "", std::string("# 議題\n議題")}, "learning");
    set_paper_review_info("lim-sess", Json{{"voiceCount", 0}, {"countCapped", false}, {"samples", Json::array()},
                                           {"fixSuggestions", Json::array({Json{{"title", "t"}, {"reason", "r"}, {"suggestedChange", "s"}}})}});
    std::string text8000;
    for (int i = 0; i < 8000; ++i) text8000 += "あ";
    auto ok = call(app.router, "POST", "/api/flow/lim-sess/paper/fix-suggestion/apply", json_stringify(Json{{"suggestionIndex", 0}, {"text", text8000}}));
    CHECK_EQ(ok.status, 200);
    // 巨大な suggestionIndex・負の値・小数
    for (const char* idx : {"1e300", "-1", "0.5"}) {
        auto r = call(app.router, "POST", "/api/flow/lim-sess/paper/fix-suggestion/apply", std::string(R"({"suggestionIndex":)") + idx + "}");
        CHECK(r.status == 404 || r.status == 409);
    }
    // 巨大なクエリ・不正な session id でも落ちない
    auto st = call(app.router, "GET", "/api/flow/" + std::string(4096, 'z') + "/status?since=" + std::string(1000, '9'));
    CHECK_EQ(st.status, 200);
    auto ls = call(app.router, "GET", "/api/flow/sessions?limit=" + std::string(500, '9') + "&offset=-" + std::string(500, '9'));
    CHECK_EQ(ls.status, 200);
    CHECK_EQ(body_json(ls)["limit"].get<int>(), 200);
    CHECK_EQ(body_json(ls)["offset"].get<int>(), 0);
}

TEST_CASE("背景実行: 草案を構築 → 下書きとして一覧に出る → 承認で議論が始まる") {
    App app;
    env::set_for_test("DISCUTERE_FLOW_ROUNDS", "1");
    env::set_for_test("DISCUTERE_FLOW_TURNS_PER_ROUND", "1");
    env::set_for_test("DISCUTERE_FLOW_PERSONA_COUNT", "2");
    config::reset_config_for_test();
    auto res = call(app.router, "POST", "/api/flow/start",
                    json_stringify(Json{{"flow", "議論"}, {"gameTitle", "テストゲーム"}, {"discussionTheme", "周回の報酬"}, {"tags", Json::array({"運用"})}, {"rounds", 1}}));
    CHECK_EQ(res.status, 200);
    const Json body = body_json(res);
    CHECK(body["review"].get<bool>());
    const std::string sid = body["sessionId"].get<std::string>();
    CHECK(web::wait_flow_web_idle_for_test(std::chrono::seconds(120)));
    auto paper = body_json(call(app.router, "GET", "/api/flow/" + sid + "/paper"));
    CHECK(paper["ready"].get<bool>());
    CHECK(paper["error"].is_null());
    CHECK(paper["paper"]["bodyMd"].get<std::string>().find("周回の報酬") != std::string::npos);
    CHECK_EQ(paper["settings"]["rounds"].get<int>(), 1);
    CHECK_EQ(paper["rev"].get<int>(), 1);
    auto list = body_json(call(app.router, "GET", "/api/flow/sessions?state=draft"));
    bool listed = false;
    for (const auto& s : list["sessions"])
        if (s["sessionId"] == sid) listed = true;
    CHECK(listed);
    // 承認 → 背景で議論 (LLM は応答しないが落ちずに終わる) → status は done
    auto ap = call(app.router, "POST", "/api/flow/" + sid + "/paper/approve", "{}");
    CHECK_EQ(ap.status, 200);
    CHECK(web::wait_flow_web_idle_for_test(std::chrono::seconds(300)));
    auto status = body_json(call(app.router, "GET", "/api/flow/" + sid + "/status"));
    CHECK(status["done"].get<bool>());
    const auto snap = get_paper_snapshot(sid);
    CHECK(snap.has_value());
    CHECK_EQ(snap->status, std::string("started"));
    // 承認後は編集エントリが無い
    CHECK_EQ(call(app.router, "POST", "/api/flow/" + sid + "/paper/approve", "{}").status, 409);
    for (const char* k : {"DISCUTERE_FLOW_ROUNDS", "DISCUTERE_FLOW_TURNS_PER_ROUND", "DISCUTERE_FLOW_PERSONA_COUNT"}) env::set_for_test(k, std::nullopt);
    config::reset_config_for_test();
}

TEST_CASE("背景実行: キューが満杯なら 503、空けば受け付ける / DELETE は進行中の議論を止める") {
    App app;
    env::set_for_test("DISCUTERE_FLOW_PAPER_REVIEW_WEB_CANONICAL", "false");
    env::set_for_test("DISCUTERE_FLOW_INFOGATE_ENABLED", "false");
    config::reset_config_for_test();
    // 背景のプールを塞ぐ (失敗で抜けても必ず解放して、塞いだ仕事の終わりを待つ)
    struct Blocker {
        std::mutex m;
        std::condition_variable cv;
        bool release = false;
        std::atomic<int> pending{0};
        void open() {
            {
                std::lock_guard lock(m);
                release = true;
            }
            cv.notify_all();
            while (pending.load() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        ~Blocker() { open(); }
    } blocker;
    auto& ex = background_executor();
    // 他の仕事 (前のケースの名残など) が終わってから塞ぐ (途中で空きができないように)
    for (int i = 0; i < 6000 && (ex.running() > 0 || ex.queued() > 0); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    std::atomic<int> started{0};
    auto fill = [&] {
        for (int i = 0; i < 10000; ++i) {
            ++blocker.pending;
            const bool ok = ex.submit([&blocker, &started] {
                ++started;
                {
                    std::unique_lock lock(blocker.m);
                    blocker.cv.wait(lock, [&] { return blocker.release; });
                }
                --blocker.pending;
            });
            if (!ok) {
                --blocker.pending;
                break;
            }
        }
    };
    fill();
    // 全 worker が塞がるまで待ってから、空いた分をもう一度埋める
    // background_executor は 8 スレッド
    for (int i = 0; i < 2000 && started.load() < 8; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const std::string req = json_stringify(Json{{"theme", "満杯のテーマ"}, {"flow", "議論"}});
    // 他のモジュールの仕事が終わって空きができる競合があるので、埋め直して数回試す
    http::Response busy;
    for (int attempt = 0; attempt < 20; ++attempt) {
        fill();
        busy = call(app.router, "POST", "/api/flow/start", req);
        if (busy.status == 503) break;
    }
    CHECK_EQ(busy.status, 503);
    CHECK_EQ(body_json(busy)["ok"].get<bool>(), false);
    blocker.open();
    // 空いたら受け付ける (LLM が応答しないので議論は早く終わる)
    auto okres = call(app.router, "POST", "/api/flow/start", req);
    CHECK_EQ(okres.status, 200);
    const std::string sid = body_json(okres)["sessionId"].get<std::string>();
    // DELETE (行の有無に関わらず) で止める指示が入り、背景の仕事は終わる
    const auto del = call(app.router, "DELETE", "/api/flow/" + sid);
    CHECK(del.status == 200 || del.status == 404);
    CHECK(web::wait_flow_web_idle_for_test(std::chrono::seconds(300)));
    env::set_for_test("DISCUTERE_FLOW_PAPER_REVIEW_WEB_CANONICAL", std::nullopt);
    env::set_for_test("DISCUTERE_FLOW_INFOGATE_ENABLED", std::nullopt);
    config::reset_config_for_test();
}

int main() { return di::test::run_all(); }
