// worker-pool/pool.ts / client.ts / api/worker.ts / api/worker-pool-control.ts の移植の検証 (TS テスト無し)。
// 偽の Lictor sidecar (/v1/keys) をテスト内の HTTP サーバで立て、ワーカーの起動・停止は差し替える。
#include "check.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

#include <nlohmann/json.hpp>

#include "api/worker.hpp"
#include "api/worker_pool_control.hpp"
#include "base/crypto.hpp"
#include "base/strings.hpp"
#include "infra/http_client.hpp"
#include "infra/http_server.hpp"
#include "persona_engine/llm/mock.hpp"
#include "persona_engine/worker_pool/client.hpp"
#include "persona_engine/worker_pool/pool.hpp"
#include "w2i_support.hpp"

using namespace di;
using namespace di::persona_engine;
using namespace di::persona_engine::worker_pool;
using namespace std::chrono_literals;

namespace {

/** 偽の Lictor sidecar。"\r" を受けたら (設定により) callback で発話を返す。 */
struct FakeLictor {
    std::shared_ptr<http::Router> router = std::make_shared<http::Router>();
    std::unique_ptr<http::Server> server;
    std::mutex mutex;
    std::vector<std::string> keys;
    std::atomic<int> status{200};
    std::function<void(const std::string& req_id)> on_submit;
    std::string last_req_id;
    FakeLictor() {
        router->post("/v1/keys", [this](http::Request& req) {
            const auto body = req.json();
            const std::string data = body ? (*body)["data"].get<std::string>() : "";
            std::function<void(const std::string&)> cb;
            std::string rid;
            {
                std::lock_guard lock(mutex);
                keys.push_back(data);
                if (str::starts_with(data, "[TURN] ")) last_req_id = str::split(data, " ")[1];
                if (data == "\r") {
                    cb = on_submit;
                    rid = last_req_id;
                }
            }
            if (status.load() != 200) return http::Response::text(std::string(300, 'e'), status.load());
            if (cb) std::thread([cb, rid] { std::this_thread::sleep_for(20ms); cb(rid); }).detach();
            return http::Response::json(Json{{"ok", true}});
        });
        http::ServerOptions o;
        o.host = "127.0.0.1";
        o.port = 0;
        server = std::make_unique<http::Server>(o, router);
        server->start();
    }
    ~FakeLictor() { server->stop(); }
    int port() const { return server->bound_port(); }
};

struct Spawns {
    std::mutex mutex;
    std::vector<std::string> spawned;
    std::vector<std::pair<std::optional<std::int64_t>, std::optional<std::int64_t>>> killed;
    std::int64_t next_pid = 1000;
};

WorkerPoolConfig make_cfg(const std::string& dir) {
    WorkerPoolConfig cfg;
    cfg.workspace = "debate";
    cfg.callbackBaseUrl = "http://127.0.0.1:3110";
    cfg.workerCwd = dir;
    cfg.turnTimeoutMs = 1500;
    cfg.registerSettleMs = 30;
    cfg.turnsDir = "turns";
    cfg.promptsDir = fs::join(dir, "prompts");
    cfg.workers = {{"con-opus", "否定派", "claude", "claude-opus-4"}, {"pro-gpt", "肯定派", "codex", "gpt-5"}};
    return cfg;
}

std::shared_ptr<WorkerPool> make_pool(const std::string& dir, std::shared_ptr<Spawns> sp, std::vector<std::string>* warns = nullptr) {
    WorkerProcessOps ops;
    ops.spawn = [sp](const WorkerConfig& w, const std::string&, const WorkerPoolConfig&) -> std::optional<std::int64_t> {
        std::lock_guard lock(sp->mutex);
        sp->spawned.push_back(w.id);
        return sp->next_pid++;
    };
    ops.kill = [sp](std::optional<std::int64_t> a, std::optional<std::int64_t> b) {
        std::lock_guard lock(sp->mutex);
        sp->killed.emplace_back(a, b);
    };
    PoolLogger log;
    log.info = [](const std::string&) {};
    log.warn = [warns](const std::string& m) {
        if (warns) warns->push_back(m);
    };
    return std::make_shared<WorkerPool>(make_cfg(dir), dir, log, ops);
}

void wait_ready(WorkerPool& pool, const std::string& id) {
    for (int i = 0; i < 300 && !pool.is_ready(id); ++i) std::this_thread::sleep_for(5ms);
}

http::Request request(const std::string& method, const std::string& path, const std::string& remote, std::string body = "") {
    http::Request r;
    r.method = method;
    r.path = path;
    r.target = path;
    r.remote_address = remote;
    r.body = std::move(body);
    return r;
}

} // namespace

TEST_CASE("pool: 起動・一覧・register の settle・停止") {
    const auto dir = w2i::temp_dir("pool");
    auto sp = std::make_shared<Spawns>();
    std::vector<std::string> warns;
    auto pool = make_pool(dir, sp, &warns);
    CHECK(!pool->has_worker("con-opus"));
    CHECK(!pool->start_worker("ghost"));
    CHECK(pool->start_worker("con-opus"));
    CHECK(pool->start_worker("con-opus"));  // 起動済はスキップ
    CHECK_EQ(sp->spawned.size(), std::size_t(1));
    CHECK(fs::is_file(fs::join(dir, "prompts/con-opus.md")));
    CHECK(fs::is_dir(fs::join(dir, "turns")));  // 相対は cwd から
    pool->start();
    CHECK_EQ(sp->spawned.size(), std::size_t(2));

    auto listed = pool->list_configured();
    CHECK_EQ(listed.size(), std::size_t(2));
    CHECK(listed[0].running && !listed[0].registered && !listed[0].settling);
    CHECK_EQ(to_json(listed[0]).dump(),
             std::string(R"({"id":"con-opus","role":"否定派","provider":"claude","model":"claude-opus-4","running":true,"registered":false,"settling":false,"busy":false,"pid":1000,"port":null})"));

    CHECK(!pool->register_port("ghost", 1234));
    CHECK_EQ(warns.back(), std::string("register from unknown worker ghost"));
    CHECK(pool->register_port("con-opus", 4321, 77));
    CHECK(pool->list_configured()[0].settling);
    CHECK(!pool->is_ready("con-opus"));
    wait_ready(*pool, "con-opus");
    CHECK(pool->is_ready("con-opus"));
    CHECK(pool->list_configured()[0].registered);
    CHECK_EQ(to_json(pool->snapshot()[0]).dump(),
             std::string(R"({"id":"con-opus","provider":"claude","model":"claude-opus-4","port":4321,"settling":false,"busy":false,"pid":1000,"lictorPid":77})"));

    // 再 register は settle をやり直す / 停止したワーカーの古いタイマーは効かない
    CHECK(pool->register_port("pro-gpt", 5555));
    CHECK(pool->stop_worker("pro-gpt"));
    CHECK(!pool->stop_worker("pro-gpt"));
    std::this_thread::sleep_for(80ms);
    CHECK(!pool->is_ready("pro-gpt"));
    CHECK(pool->get_worker_config("pro-gpt").has_value());
    CHECK(!pool->get_worker_config("ghost"));

    pool->stop();
    CHECK(!pool->has_worker("con-opus"));
    CHECK_EQ(sp->killed.size(), std::size_t(2));
    CHECK_EQ(*sp->killed.back().second, std::int64_t(77));
}

TEST_CASE("pool: dispatch (注入 2 段・ターン JSON・callback・busy・期限・/v1/keys 失敗・stop)") {
    const auto dir = w2i::temp_dir("dispatch");
    FakeLictor lictor;
    auto sp = std::make_shared<Spawns>();
    std::vector<std::string> warns;
    auto pool = make_pool(dir, sp, &warns);
    pool->start();

    CHECK_THROWS(pool->dispatch("ghost", {"r1", "s", "p"}));
    try {
        pool->dispatch("con-opus", {"r1", "s", "p"});
        CHECK(false);
    } catch (const std::runtime_error& e) {
        CHECK_EQ(std::string(e.what()), std::string("worker con-opus not registered (no port)"));
    }
    pool->register_port("con-opus", lictor.port());
    try {
        pool->dispatch("con-opus", {"r1", "s", "p"});
        CHECK(false);
    } catch (const std::runtime_error& e) {
        CHECK(str::contains(e.what(), "still settling"));
    }
    wait_ready(*pool, "con-opus");
    CHECK_THROWS(pool->dispatch("con-opus", {"../evil", "s", "p"}));  // ファイル名に使う reqId は安全な文字だけ

    TokenUsage usage;
    usage.input_tokens = 10;
    lictor.on_submit = [pool, usage](const std::string& rid) { pool->on_utterance(rid, "con-opus", "発話です", usage); };
    const auto res = pool->dispatch("con-opus", {"req-1", "SYS", "PROMPT"});
    CHECK_EQ(res.text, std::string("発話です"));
    CHECK_EQ(*res.usage->input_tokens, 10.0);
    {
        std::lock_guard lock(lictor.mutex);
        CHECK_EQ(lictor.keys.size(), std::size_t(2));
        CHECK(str::starts_with(lictor.keys[0], "[TURN] req-1 "));
        CHECK(str::ends_with(lictor.keys[0], "/turns/req-1.json"));
        CHECK_EQ(lictor.keys[1], std::string("\r"));
    }
    const auto turn = fs::read_file(fs::join(dir, "turns/req-1.json"));
    CHECK_EQ(*turn, std::string("{\n  \"reqId\": \"req-1\",\n  \"workerId\": \"con-opus\",\n  \"system\": \"SYS\",\n  \"prompt\": \"PROMPT\"\n}"));
    CHECK(!pool->list_configured()[0].busy);

    // busy: 走行中の 2 本目は拒否
    lictor.on_submit = [](const std::string&) {};
    auto first = std::async(std::launch::async, [&] {
        try {
            pool->dispatch("con-opus", {"req-2", "", ""});
            return std::string("ok");
        } catch (const std::exception& e) {
            return std::string(e.what());
        }
    });
    std::this_thread::sleep_for(300ms);
    try {
        pool->dispatch("con-opus", {"req-3", "", ""});
        CHECK(false);
    } catch (const std::runtime_error& e) {
        CHECK_EQ(std::string(e.what()), std::string("worker con-opus busy"));
    }
    // 期限: callback が来なければ turn timeout、busy は戻る
    CHECK_EQ(first.get(), std::string("worker con-opus turn timeout (1500ms)"));
    CHECK(!pool->list_configured()[0].busy);
    pool->on_utterance("req-2", "con-opus", "late");  // 期限切れの callback は warn だけ
    CHECK_EQ(warns.back(), std::string("utterance for unknown/expired reqId req-2"));

    // /v1/keys が失敗したら本文 120 文字までを含むエラー、busy は戻る
    lictor.status = 500;
    try {
        pool->dispatch("con-opus", {"req-4", "", ""});
        CHECK(false);
    } catch (const std::runtime_error& e) {
        CHECK_EQ(std::string(e.what()), "/v1/keys 500: " + std::string(120, 'e'));
    }
    CHECK(!pool->list_configured()[0].busy);
    lictor.status = 200;

    // stop: 待っているターンは "pool stopped"
    auto waiting = std::async(std::launch::async, [&] {
        try {
            pool->dispatch("con-opus", {"req-5", "", ""});
            return std::string("ok");
        } catch (const std::exception& e) {
            return std::string(e.what());
        }
    });
    std::this_thread::sleep_for(300ms);
    pool->stop();
    CHECK_EQ(waiting.get(), std::string("pool stopped"));
}

TEST_CASE("pool: Lictor に繋がらなければ fetch failed") {
    const auto dir = w2i::temp_dir("noconn");
    auto pool = make_pool(dir, std::make_shared<Spawns>());
    pool->start();
    pool->register_port("con-opus", 1);  // 誰も listen していない
    wait_ready(*pool, "con-opus");
    try {
        pool->dispatch("con-opus", {"req-x", "", ""});
        CHECK(false);
    } catch (const std::runtime_error& e) {
        CHECK_EQ(std::string(e.what()), std::string("fetch failed"));
    }
    CHECK(!pool->list_configured()[0].busy);
}

TEST_CASE("client: ルーティング・JSON 包み・fallback の model") {
    const auto dir = w2i::temp_dir("client");
    FakeLictor lictor;
    auto pool = make_pool(dir, std::make_shared<Spawns>());
    WorkerPoolClient bare(pool);
    LLMInvokeArgs a;
    a.prompt = "p";
    a.conversationOnly = true;
    CHECK_EQ(bare.invoke(a).error, std::string("Persistent worker cannot accept external conversation-only requests"));
    a.conversationOnly = false;
    CHECK_EQ(bare.invoke(a).error, std::string("worker-pool: personaId 未指定 (ルーティング不能)"));
    a.personaId = "";
    CHECK_EQ(bare.invoke(a).error, std::string("worker-pool: personaId 未指定 (ルーティング不能)"));
    a.personaId = "con-opus";
    CHECK_EQ(bare.invoke(a).error, std::string("worker-pool: worker 'con-opus' 未起動 かつ fallback 無し"));

    auto fb = std::make_shared<MockLLMClient>(std::vector<MockResponder>{}, "fallback");
    WorkerPoolClient with_fb(pool, fb);
    CHECK_EQ(with_fb.invoke(a).text, std::string("fallback"));
    CHECK_EQ(*fb->last_invocation()->model, std::string("claude-opus-4"));
    a.personaId = "pro-gpt";
    a.model = "claude-x";
    with_fb.invoke(a);
    CHECK(!fb->last_invocation()->model);  // 非 claude モデルは渡さない
    a.personaId = "unknown";
    with_fb.invoke(a);
    CHECK_EQ(*fb->last_invocation()->model, std::string("claude-x"));  // 定義が無ければ args.model
    a.model = "GPT";
    with_fb.invoke(a);
    CHECK(!fb->last_invocation()->model);

    pool->start();
    pool->register_port("con-opus", lictor.port());
    wait_ready(*pool, "con-opus");
    a.personaId = "con-opus";
    a.system = "SYS";
    lictor.on_submit = [pool](const std::string& rid) { pool->on_utterance(rid, "con-opus", "  言いたいこと \n"); };
    auto r = bare.invoke(a);
    CHECK(r.ok);
    CHECK_EQ(r.text, std::string(R"({"action":"post_utterance","text":"言いたいこと"})"));
    lictor.on_submit = [pool](const std::string& rid) { pool->on_utterance(rid, "con-opus", "   "); };
    CHECK_EQ(bare.invoke(a).text, std::string(R"j({"action":"skip","reasoning":"worker skip (空応答)"})j"));
    lictor.status = 500;
    CHECK(str::starts_with(bare.invoke(a).error, "worker-pool: /v1/keys 500"));
    CHECK_EQ(with_fb.invoke(a).text, std::string("fallback"));  // 失敗 → fallback
    pool->stop();
}

TEST_CASE("api/worker: loopback 限定 (境界)・503・入力検査・usage") {
    api::set_worker_pool(nullptr);
    auto router = std::make_shared<http::Router>();
    router->mount("/internal", api::worker_routes());
    for (const std::string remote : {"192.168.1.10", "::ffff:192.168.1.10", "10.0.0.1", "", "127.0.0.2", "::2"}) {
        auto req = request("POST", "/internal/worker/register", remote, "{}");
        const auto res = router->handle(req);
        CHECK_EQ(res.status, 403);
        CHECK_EQ(res.body, std::string(R"({"error":"loopback only"})"));
    }
    for (const std::string remote : {"127.0.0.1", "::1", "::ffff:127.0.0.1"}) {
        auto req = request("POST", "/internal/worker/utterance", remote, "{}");
        const auto res = router->handle(req);
        CHECK_EQ(res.status, 503);
        CHECK_EQ(res.body, std::string(R"({"error":"worker pool not initialized"})"));
    }

    const auto dir = w2i::temp_dir("api");
    auto pool = make_pool(dir, std::make_shared<Spawns>());
    pool->start();
    api::set_worker_pool(pool);
    auto post = [&](const std::string& path, const std::string& body) {
        auto req = request("POST", "/internal/worker/" + path, "127.0.0.1", body);
        return router->handle(req);
    };
    const std::string need = R"({"error":"workerId (string) and lictorPort (number) required"})";
    CHECK_EQ(post("register", "not json").body, need);
    CHECK_EQ(post("register", "[1]").body, need);
    CHECK_EQ(post("register", R"({"workerId":"con-opus"})").body, need);
    CHECK_EQ(post("register", R"({"workerId":1,"lictorPort":1})").body, need);
    CHECK_EQ(post("register", R"({"workerId":"con-opus","lictorPort":"1"})").status, 400);
    CHECK_EQ(post("register", R"({"workerId":"con-opus","lictorPort":1.5})").status, 400);
    CHECK_EQ(post("register", R"({"workerId":"con-opus","lictorPort":65536})").status, 400);
    CHECK_EQ(post("register", R"({"workerId":"ghost","lictorPort":65535})").body, std::string(R"({"ok":false})"));
    CHECK_EQ(post("register", R"({"workerId":"con-opus","lictorPort":65535,"lictorPid":-3})").body, std::string(R"({"ok":true})"));
    CHECK(!pool->snapshot()[0].lictorPid);
    CHECK_EQ(post("register", R"({"workerId":"con-opus","lictorPort":4000,"lictorPid":42})").body, std::string(R"({"ok":true})"));
    CHECK_EQ(*pool->snapshot()[0].lictorPid, std::int64_t(42));
    CHECK_EQ(*pool->snapshot()[0].port, std::int64_t(4000));

    CHECK_EQ(post("utterance", "{").body, std::string(R"({"error":"reqId (string) required"})"));
    CHECK_EQ(post("utterance", R"({"reqId":5})").status, 400);
    CHECK_EQ(post("utterance", R"({"reqId":"nope","text":7})").body, std::string(R"({"ok":true})"));

    // 深すぎる JSON は読まない (壊れた入力扱い)
    CHECK_EQ(post("utterance", std::string(100, '[') + std::string(100, ']')).status, 400);

    // usage は既知の数値だけ
    CHECK(!api::pick_usage(Json::array()));
    CHECK(!api::pick_usage(Json{{"cost_usd", 1.0}, {"model", "m"}}));
    const auto u = api::pick_usage(Json{{"input_tokens", 3}, {"output_tokens", "x"}, {"cost_usd", 0.5}, {"model", ""}, {"extra", 1}});
    CHECK(u.has_value());
    CHECK_EQ(*u->input_tokens, 3.0);
    CHECK(!u->output_tokens);
    CHECK_EQ(*u->cost_usd, 0.5);
    CHECK(!u->model);
    CHECK_EQ(*api::pick_usage(Json{{"cache_read_input_tokens", 1}, {"model", "claude"}})->model, std::string("claude"));
    pool->stop();
    api::set_worker_pool(nullptr);
}

TEST_CASE("api/worker: 本文の上限 (1 MiB ちょうど / +1) を実サーバで") {
    api::set_worker_pool(nullptr);
    auto router = std::make_shared<http::Router>();
    router->mount("/internal", api::worker_routes());
    http::ServerOptions o;
    o.port = 0;
    http::Server server(o, router);
    server.start();
    auto send = [&](std::size_t n) {
        http::ClientRequest r;
        r.method = "POST";
        r.url = "http://127.0.0.1:" + std::to_string(server.bound_port()) + "/internal/worker/utterance";
        r.body = std::string(n, ' ');
        r.allow_private_network = true;
        return http::fetch(r).status;
    };
    CHECK_EQ(send(http::kDefaultMaxBody), 503);
    CHECK_EQ(send(http::kDefaultMaxBody + 1), 413);
    server.stop();
}

TEST_CASE("api/worker-pool-control: 状態・起動停止・404・画面") {
    api::set_worker_pool_control(nullptr);
    http::Router router;
    router.mount("/api", api::worker_pool_control_routes());
    auto call = [&](const std::string& method, const std::string& path) {
        auto req = request(method, path, "127.0.0.1");
        return router.handle(req);
    };
    CHECK_EQ(call("GET", "/api/worker-pool/status").body, std::string(R"({"enabled":false,"workers":[]})"));
    CHECK_EQ(call("POST", "/api/worker-pool/start").status, 503);
    CHECK_EQ(call("POST", "/api/worker-pool/start").body, std::string(R"j({"error":"worker pool not enabled (backend != worker-pool)"})j"));
    CHECK_EQ(call("POST", "/api/worker-pool/stop").body, std::string(R"({"error":"worker pool not enabled"})"));
    CHECK_EQ(call("POST", "/api/worker-pool/worker/x/start").status, 503);

    const auto dir = w2i::temp_dir("control");
    auto sp = std::make_shared<Spawns>();
    auto pool = make_pool(dir, sp);
    api::set_worker_pool_control(pool);
    auto status = parse_json_or_throw(call("GET", "/api/worker-pool/status").body);
    CHECK(status["enabled"].get<bool>());
    CHECK(!status["workers"][0]["running"].get<bool>());
    const auto started = call("POST", "/api/worker-pool/worker/con-opus/start");
    CHECK_EQ(started.status, 200);
    CHECK_EQ(started.body, std::string(R"({"ok":true})"));
    CHECK_EQ(call("POST", "/api/worker-pool/worker/ghost/start").status, 404);
    CHECK_EQ(call("POST", "/api/worker-pool/worker/ghost/stop").body, std::string(R"({"ok":false})"));
    CHECK_EQ(call("POST", "/api/worker-pool/worker/con-opus/stop").status, 200);
    auto all = parse_json_or_throw(call("POST", "/api/worker-pool/start").body);
    CHECK(all["ok"].get<bool>());
    CHECK(all["workers"][1]["running"].get<bool>());
    CHECK_EQ(call("POST", "/api/worker-pool/stop").body, std::string(R"({"ok":true})"));
    CHECK_EQ(sp->killed.size(), std::size_t(3));

    const auto page = call("GET", "/api/worker-pool");
    CHECK_EQ(*page.headers.get("Content-Type"), std::string("text/html; charset=UTF-8"));
    for (const auto& a : w2i::load_fixture("web_assets.json")) {
        if (a["name"] != "api/worker-pool-control.html") continue;
        CHECK_EQ(page.body.size(), a["bytes"].get<std::size_t>());
        CHECK_EQ(crypto::sha256_hex(page.body), a["sha256"].get<std::string>());
    }
    api::set_worker_pool_control(nullptr);
}

DI_TEST_MAIN()
