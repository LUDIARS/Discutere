#pragma once
// worker_pool_tests.cpp を Praesidium の静的解析 (1 単位 60 秒) に収まるよう分けた各パートが共有する準備。
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

namespace worker_pool_support {

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

} // namespace worker_pool_support
