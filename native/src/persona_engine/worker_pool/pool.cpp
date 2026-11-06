#include "persona_engine/worker_pool/pool.hpp"

#include <algorithm>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "base/executor.hpp"
#include "base/fsutil.hpp"
#include "base/log.hpp"
#include "base/strings.hpp"
#include "base/time.hpp"
#include "infra/http_client.hpp"
#include "persona_engine/worker_pool/persona_prompts.hpp"
#include "persona_engine/worker_pool/spawner.hpp"

namespace di::persona_engine::worker_pool {

namespace {

Json opt_int(const std::optional<std::int64_t>& v) { return v ? Json(*v) : Json(nullptr); }

std::string pid_text(const std::optional<std::int64_t>& v, const char* missing) {
    return v ? std::to_string(*v) : std::string(missing);
}

/** reqId はファイル名に使うので、uuid 等の安全な文字だけを許す (TS には無い防御)。 */
bool safe_req_id(const std::string& id) {
    if (id.empty() || id.size() > 128) return false;
    for (char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    return true;
}

std::string abs_dir(const std::string& dir, const std::string& cwd) {
    return fs::to_path(dir).is_absolute() ? dir : fs::join(cwd, dir);
}

} // namespace

Json to_json(const ConfiguredWorker& w) {
    Json j = Json::object();
    j["id"] = w.id;
    j["role"] = w.role;
    j["provider"] = w.provider;
    j["model"] = w.model;
    j["running"] = w.running;
    j["registered"] = w.registered;
    j["settling"] = w.settling;
    j["busy"] = w.busy;
    j["pid"] = opt_int(w.pid);
    j["port"] = opt_int(w.port);
    return j;
}

Json to_json(const WorkerSnapshot& w) {
    Json j = Json::object();
    j["id"] = w.id;
    j["provider"] = w.provider;
    j["model"] = w.model;
    j["port"] = opt_int(w.port);
    j["settling"] = w.settling;
    j["busy"] = w.busy;
    j["pid"] = opt_int(w.pid);
    j["lictorPid"] = opt_int(w.lictorPid);
    return j;
}

struct PendingTurn {
    std::string workerId;
    std::uint64_t generation = 0;
    bool done = false;
    std::optional<DispatchResult> result;
    std::string error;
};

struct WorkerPool::State {
    WorkerPoolConfig cfg;
    PoolLogger log;
    WorkerProcessOps ops;
    std::string abs_turns_dir;
    std::string abs_prompts_dir;

    mutable std::mutex mutex;
    std::condition_variable cv;
    /** 起動中のワーカー (挿入順を保つ。TS の Map と同じ並び)。 */
    std::vector<std::pair<std::uint64_t, WorkerRuntime>> workers;  // (generation, runtime)
    std::map<std::string, std::shared_ptr<PendingTurn>> pending;
    std::uint64_t next_generation = 1;

    WorkerRuntime* find(const std::string& id) {
        for (auto& [gen, rt] : workers)
            if (rt.config.id == id) return &rt;
        return nullptr;
    }
    const WorkerRuntime* find(const std::string& id) const {
        for (const auto& [gen, rt] : workers)
            if (rt.config.id == id) return &rt;
        return nullptr;
    }
    std::uint64_t generation_of(const std::string& id) const {
        for (const auto& [gen, rt] : workers)
            if (rt.config.id == id) return gen;
        return 0;
    }

    void ensure_dirs() {
        if (!fs::ensure_dir(abs_turns_dir)) throw std::runtime_error("worker-pool: cannot create turns dir");
        if (!fs::ensure_dir(abs_prompts_dir)) throw std::runtime_error("worker-pool: cannot create prompts dir");
    }

    /** 1 ワーカーを spawn して runtime に登録 (既に起動済なら何もしない)。mutex を持って呼ぶ。 */
    void spawn_one(const WorkerConfig& worker) {
        if (find(worker.id)) return;
        const std::string prompt_path = fs::join(abs_prompts_dir, worker.id + ".md");
        if (!fs::write_file_atomic(prompt_path, build_standing_prompt(worker)))
            throw std::runtime_error("worker-pool: cannot write standing prompt for " + worker.id);
        // ワーカーは worker-home を cwd にして起動 (専用 .claude/settings.json を効かせる)。
        const auto pid = ops.spawn(worker, prompt_path, cfg);
        WorkerRuntime rt;
        rt.config = worker;
        rt.pid = pid;
        rt.promptPath = prompt_path;
        workers.emplace_back(next_generation++, std::move(rt));
        log.info("spawned worker " + worker.id + " (" + worker.provider + "/" + worker.model + ") pid=" + pid_text(pid, "undefined"));
    }

    /** Lictor sidecar の /v1/keys に raw キーを送る (期限・応答上限つき)。 */
    static void inject_keys(std::int64_t port, const std::string& data) {
        http::ClientRequest req;
        req.method = "POST";
        req.url = "http://127.0.0.1:" + std::to_string(port) + "/v1/keys";
        req.headers.set("content-type", "application/json");
        req.body = json_stringify(Json{{"data", data}});
        req.timeout = kInjectTimeout;
        req.connect_timeout = std::chrono::milliseconds(5'000);
        req.max_response_bytes = kInjectMaxResponseBytes;
        req.allow_private_network = true;  // 同一ホストの Lictor sidecar
        req.max_redirects = 0;
        const auto res = http::fetch(req);
        if (!res.transport_ok) throw std::runtime_error("fetch failed");
        if (!res.ok()) throw std::runtime_error("/v1/keys " + std::to_string(res.status) + ": " + str::js_slice(res.body, 0, 120));
    }
};

WorkerPool::WorkerPool(WorkerPoolConfig cfg, std::string cwd, std::optional<PoolLogger> logger, std::optional<WorkerProcessOps> ops)
    : state_(std::make_shared<State>()) {
    state_->abs_turns_dir = abs_dir(cfg.turnsDir, cwd);
    state_->abs_prompts_dir = abs_dir(cfg.promptsDir, cwd);
    state_->cfg = std::move(cfg);
    if (logger && logger->info && logger->warn) {
        state_->log = std::move(*logger);
    } else {
        state_->log.info = [](const std::string& m) { log::info("[worker-pool] " + m); };
        state_->log.warn = [](const std::string& m) { log::warn("[worker-pool] " + m); };
    }
    if (ops && ops->spawn && ops->kill) {
        state_->ops = std::move(*ops);
    } else {
        state_->ops.spawn = [](const WorkerConfig& worker, const std::string& prompt_path, const WorkerPoolConfig& c) {
            return spawn_worker(SpawnWorkerArgs{worker, prompt_path, c.workerCwd, c}).pid;
        };
        state_->ops.kill = [](std::optional<std::int64_t> pid, std::optional<std::int64_t> lictor_pid) { kill_worker(pid, lictor_pid); };
    }
}

WorkerPool::~WorkerPool() {
    // プロセスは残す (TS と同じく kill は stop() の責務)。タイマーと待ちだけ片付ける。
    std::vector<Scheduler::Id> timers;
    {
        std::lock_guard lock(state_->mutex);
        for (auto& [gen, rt] : state_->workers)
            if (rt.settleTimer) timers.push_back(*rt.settleTimer);
        for (auto& [id, p] : state_->pending) {
            p->done = true;
            p->error = "pool stopped";
        }
        state_->pending.clear();
    }
    state_->cv.notify_all();
    for (auto t : timers) global_scheduler().cancel(t);
}

void WorkerPool::start() {
    std::lock_guard lock(state_->mutex);
    state_->ensure_dirs();
    for (const auto& worker : state_->cfg.workers) state_->spawn_one(worker);
}

bool WorkerPool::start_worker(const std::string& id) {
    std::lock_guard lock(state_->mutex);
    const auto it = std::find_if(state_->cfg.workers.begin(), state_->cfg.workers.end(), [&](const WorkerConfig& w) { return w.id == id; });
    if (it == state_->cfg.workers.end()) return false;
    state_->ensure_dirs();
    state_->spawn_one(*it);
    return true;
}

bool WorkerPool::stop_worker(const std::string& id) {
    WorkerRuntime rt;
    {
        std::lock_guard lock(state_->mutex);
        auto it = std::find_if(state_->workers.begin(), state_->workers.end(), [&](const auto& e) { return e.second.config.id == id; });
        if (it == state_->workers.end()) return false;
        rt = it->second;
        state_->workers.erase(it);
    }
    if (rt.settleTimer) global_scheduler().cancel(*rt.settleTimer);
    state_->ops.kill(rt.pid, rt.lictorPid);
    state_->log.info("stopped worker " + id + " (pid=" + pid_text(rt.pid, "undefined") + " lictorPid=" + pid_text(rt.lictorPid, "-") + ")");
    return true;
}

std::vector<ConfiguredWorker> WorkerPool::list_configured() const {
    std::lock_guard lock(state_->mutex);
    std::vector<ConfiguredWorker> out;
    for (const auto& w : state_->cfg.workers) {
        const WorkerRuntime* rt = state_->find(w.id);
        ConfiguredWorker c;
        c.id = w.id;
        c.role = w.role;
        c.provider = w.provider;
        c.model = w.model;
        c.running = rt != nullptr;
        c.registered = rt && rt->readyAt.has_value();
        c.settling = rt && rt->lictorPort.has_value() && !rt->readyAt;
        c.busy = rt && rt->busy;
        if (rt) {
            c.pid = rt->pid;
            c.port = rt->lictorPort;
        }
        out.push_back(std::move(c));
    }
    return out;
}

bool WorkerPool::has_worker(const std::string& id) const {
    std::lock_guard lock(state_->mutex);
    return state_->find(id) != nullptr;
}

std::optional<WorkerConfig> WorkerPool::get_worker_config(const std::string& id) const {
    std::lock_guard lock(state_->mutex);
    for (const auto& w : state_->cfg.workers)
        if (w.id == id) return w;
    return std::nullopt;
}

bool WorkerPool::is_ready(const std::string& id) const {
    std::lock_guard lock(state_->mutex);
    const auto* rt = state_->find(id);
    return rt && rt->readyAt.has_value();
}

bool WorkerPool::register_port(const std::string& worker_id, std::int64_t lictor_port, std::optional<std::int64_t> lictor_pid) {
    std::optional<Scheduler::Id> old_timer;
    const long long settle_ms = state_->cfg.registerSettleMs;
    {
        std::lock_guard lock(state_->mutex);
        WorkerRuntime* rt = state_->find(worker_id);
        if (!rt) {
            state_->log.warn("register from unknown worker " + worker_id);
            return false;
        }
        old_timer = rt->settleTimer;
        rt->lictorPort = lictor_port;
        rt->lictorPid = lictor_pid;
        rt->readyAt.reset();
        // register.mjs の POST が来た時点でワーカーの TUI はまだ「登録完了」を生成中。
        // registerSettleMs 経過後に readyAt を設定して dispatch を解禁する (busy な TUI へのキー注入を防ぐ)。
        const std::uint64_t generation = state_->generation_of(worker_id);
        std::weak_ptr<State> weak = state_;
        auto id_holder = std::make_shared<Scheduler::Id>(0);
        const Scheduler::Id timer = global_scheduler().after(
            std::chrono::milliseconds(settle_ms > 0 ? settle_ms : 0), [weak, worker_id, generation, settle_ms, id_holder] {
                auto s = weak.lock();
                if (!s) return;
                std::lock_guard lock(s->mutex);
                WorkerRuntime* r = s->find(worker_id);
                // 停止・再起動・再 register で別のタイマーに置き換わっていれば何もしない。
                if (!r || s->generation_of(worker_id) != generation || r->settleTimer != *id_holder) return;
                r->readyAt = time::now_ms();
                r->settleTimer.reset();
                s->log.info("worker " + worker_id + " idle-ready (settled after " + std::to_string(settle_ms) + "ms)");
            });
        *id_holder = timer;
        rt->settleTimer = timer;
        state_->log.info("worker " + worker_id + " registered port=" + std::to_string(lictor_port) +
                         " lictorPid=" + pid_text(lictor_pid, "-") + " — settling " + std::to_string(settle_ms) + "ms");
    }
    if (old_timer) global_scheduler().cancel(*old_timer);
    return true;
}

DispatchResult WorkerPool::dispatch(const std::string& worker_id, const DispatchPayload& payload) {
    if (!safe_req_id(payload.reqId)) throw std::runtime_error("invalid reqId");
    auto turn = std::make_shared<PendingTurn>();
    std::int64_t port = 0;
    {
        std::lock_guard lock(state_->mutex);
        WorkerRuntime* rt = state_->find(worker_id);
        if (!rt) throw std::runtime_error("unknown worker " + worker_id);
        if (!rt->lictorPort) throw std::runtime_error("worker " + worker_id + " not registered (no port)");
        if (!rt->readyAt)
            throw std::runtime_error("worker " + worker_id + " still settling after register — retry after registerSettleMs");
        if (rt->busy) throw std::runtime_error("worker " + worker_id + " busy");
        if (state_->pending.count(payload.reqId)) throw std::runtime_error("duplicate reqId " + payload.reqId);
        // 注入中に callback が戻っても取りこぼさないよう、待ちは注入の前に登録する。
        rt->busy = true;
        port = *rt->lictorPort;
        turn->workerId = worker_id;
        turn->generation = state_->generation_of(worker_id);
        state_->pending[payload.reqId] = turn;
    }
    auto release = [&] {
        std::lock_guard lock(state_->mutex);
        auto it = state_->pending.find(payload.reqId);
        if (it != state_->pending.end() && it->second == turn) state_->pending.erase(it);
        if (WorkerRuntime* rt = state_->find(worker_id); rt && state_->generation_of(worker_id) == turn->generation) rt->busy = false;
    };

    try {
        const std::string turn_path = fs::join(state_->abs_turns_dir, payload.reqId + ".json");
        Json body = Json::object();
        body["reqId"] = payload.reqId;
        body["workerId"] = worker_id;
        body["system"] = payload.system;
        body["prompt"] = payload.prompt;
        if (!fs::write_file_atomic(turn_path, json_stringify_pretty(body, 2)))
            throw std::runtime_error("cannot write turn file");
        // worker の cwd は worker-home なので turnsDir を相対で渡すと解決できない。
        // 絶対パス (forward slash 正規化) で注入する。worker は Read ツールで読む。
        const std::string turn_abs = str::replace_all(turn_path, "\\", "/");
        // 本文と Enter(CR) を分けて送る。1 write だと bracketed paste 扱いで CR が改行化し submit されない。
        State::inject_keys(port, "[TURN] " + payload.reqId + " " + turn_abs);
        std::this_thread::sleep_for(std::chrono::milliseconds(SUBMIT_DELAY_MS));
        State::inject_keys(port, "\r");
    } catch (...) {
        release();
        throw;
    }

    std::unique_lock lock(state_->mutex);
    const bool settled = state_->cv.wait_for(lock, std::chrono::milliseconds(std::max<long long>(0, state_->cfg.turnTimeoutMs)),
                                             [&] { return turn->done; });
    if (!settled) {
        lock.unlock();
        release();
        throw std::runtime_error("worker " + worker_id + " turn timeout (" + std::to_string(state_->cfg.turnTimeoutMs) + "ms)");
    }
    if (turn->result) return *turn->result;
    throw std::runtime_error(turn->error.empty() ? std::string("pool stopped") : turn->error);
}

void WorkerPool::on_utterance(const std::string& req_id, const std::string& /*worker_id*/, const std::string& text,
                              std::optional<TokenUsage> usage) {
    {
        std::lock_guard lock(state_->mutex);
        auto it = state_->pending.find(req_id);
        if (it == state_->pending.end()) {
            state_->log.warn("utterance for unknown/expired reqId " + req_id);
            return;
        }
        auto turn = it->second;
        state_->pending.erase(it);
        if (WorkerRuntime* rt = state_->find(turn->workerId); rt && state_->generation_of(turn->workerId) == turn->generation)
            rt->busy = false;
        turn->result = DispatchResult{text, std::move(usage)};
        turn->done = true;
    }
    state_->cv.notify_all();
}

void WorkerPool::stop() {
    std::vector<WorkerRuntime> runtimes;
    {
        std::lock_guard lock(state_->mutex);
        for (auto& [gen, rt] : state_->workers) runtimes.push_back(rt);
        state_->workers.clear();
        for (auto& [id, p] : state_->pending) {
            p->done = true;
            p->error = "pool stopped";
        }
        state_->pending.clear();
    }
    state_->cv.notify_all();
    for (const auto& rt : runtimes) {
        if (rt.settleTimer) global_scheduler().cancel(*rt.settleTimer);
        state_->ops.kill(rt.pid, rt.lictorPid);
    }
}

std::vector<WorkerSnapshot> WorkerPool::snapshot() const {
    std::lock_guard lock(state_->mutex);
    std::vector<WorkerSnapshot> out;
    for (const auto& [gen, rt] : state_->workers) {
        WorkerSnapshot s;
        s.id = rt.config.id;
        s.provider = rt.config.provider;
        s.model = rt.config.model;
        s.port = rt.lictorPort;
        s.settling = rt.lictorPort.has_value() && !rt.readyAt;
        s.busy = rt.busy;
        s.pid = rt.pid;
        s.lictorPid = rt.lictorPid;
        out.push_back(std::move(s));
    }
    return out;
}

} // namespace di::persona_engine::worker_pool
