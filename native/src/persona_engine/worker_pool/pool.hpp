#pragma once
// 常駐ワーカープール (src/persona-engine/worker-pool/pool.ts)。
//
// - start(): 全ワーカーを spawn (standing prompt を書き出して auto-inject に乗せる)。
// - register_port(): ワーカーの自己 register を受けて sidecar port を記録 (settle 後に dispatch 解禁)。
// - dispatch(): ターン JSON を書き出し、ワーカーの Lictor /v1/keys に 1 行注入。
//               発話が callback (on_utterance) で戻るまで呼んだスレッドで待つ (期限 turnTimeoutMs)。
// - on_utterance(): callback 受信 → 対応する待ちを起こす。
// - stop(): 全ワーカーを kill し、待っているターンを "pool stopped" で失敗させる。
// 状態は mutex で守る (HTTP の callback・スケジューラ・議論のスレッドから同時に触られる)。
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/json.hpp"
#include "persona_engine/llm/client.hpp"
#include "persona_engine/worker_pool/types.hpp"

namespace di::persona_engine::worker_pool {

struct PoolLogger {
    std::function<void(const std::string& msg)> info;
    std::function<void(const std::string& msg)> warn;
};

/** 1 ターンの callback 結果 (発話本文 + 任意の token usage)。 */
struct DispatchResult {
    std::string text;
    /** worker-home の send.mjs が transcript から拾った token (任意, #135)。 */
    std::optional<TokenUsage> usage;
};

struct DispatchPayload {
    std::string reqId;
    std::string system;
    std::string prompt;
};

struct ConfiguredWorker {
    std::string id;
    std::string role;
    std::string provider;
    std::string model;
    bool running = false;
    bool registered = false;
    bool settling = false;
    bool busy = false;
    std::optional<std::int64_t> pid;
    std::optional<std::int64_t> port;
};

struct WorkerSnapshot {
    std::string id;
    std::string provider;
    std::string model;
    std::optional<std::int64_t> port;
    bool settling = false;
    bool busy = false;
    std::optional<std::int64_t> pid;
    std::optional<std::int64_t> lictorPid;
};

Json to_json(const ConfiguredWorker& w);
Json to_json(const WorkerSnapshot& w);

/**
 * TUI へ本文を注入してから Enter (CR) を「別 write」で送るまでの待ち時間 (ms)。
 * 本文 + CR を 1 write で送ると Claude TUI が bracketed paste 扱いし、CR が入力欄の改行として
 * 取り込まれて submit されない。本文 → 待機 → CR の 2 段送出で確実に submit させる。
 */
inline constexpr long long SUBMIT_DELAY_MS = 150;
/** Lictor /v1/keys への 1 回の要求の期限と応答の上限。 */
inline constexpr std::chrono::milliseconds kInjectTimeout{10'000};
inline constexpr std::size_t kInjectMaxResponseBytes = 64 * 1024;

/** ワーカーの起動と停止 (テストで差し替える)。既定は spawner の spawn_worker / kill_worker。 */
struct WorkerProcessOps {
    std::function<std::optional<std::int64_t>(const WorkerConfig& worker, const std::string& prompt_path,
                                              const WorkerPoolConfig& cfg)>
        spawn;
    std::function<void(std::optional<std::int64_t> pid, std::optional<std::int64_t> lictor_pid)> kill;
};

class WorkerPool {
public:
    WorkerPool(WorkerPoolConfig cfg, std::string cwd, std::optional<PoolLogger> logger = std::nullopt,
               std::optional<WorkerProcessOps> ops = std::nullopt);
    ~WorkerPool();
    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    /** 設定された全ワーカーを spawn する (起動済はスキップ)。 */
    void start();
    /** 指定 id のワーカーを 1 体起動。config に無い id は false。 */
    bool start_worker(const std::string& id);
    /** 指定 id のワーカーを 1 体停止 (プロセス kill + runtime 除去)。 */
    bool stop_worker(const std::string& id);
    /** config に定義された全ワーカー (起動状態込み) を返す (UI 用)。 */
    std::vector<ConfiguredWorker> list_configured() const;
    bool has_worker(const std::string& id) const;
    /** personaId に対応する worker 定義 (provider/model)。config に無ければ nullopt。 */
    std::optional<WorkerConfig> get_worker_config(const std::string& id) const;
    /** dispatch 可能か (settle 完了 = TUI が idle に戻っている)。 */
    bool is_ready(const std::string& id) const;
    /** ワーカーの自己 register。port と lictorPid を記録し、settle timer を開始する。 */
    bool register_port(const std::string& worker_id, std::int64_t lictor_port,
                       std::optional<std::int64_t> lictor_pid = std::nullopt);
    /** ターンをワーカーへ投げ、発話が callback で戻るまで待つ。失敗は std::runtime_error。 */
    DispatchResult dispatch(const std::string& worker_id, const DispatchPayload& payload);
    /** callback 受信。対応する待ちを起こし、ワーカーを idle に戻す。 */
    void on_utterance(const std::string& req_id, const std::string& worker_id, const std::string& text,
                      std::optional<TokenUsage> usage = std::nullopt);
    /** 全ワーカーを kill。 */
    void stop();
    /** 診断用スナップショット。 */
    std::vector<WorkerSnapshot> snapshot() const;

    struct State;

private:
    std::shared_ptr<State> state_;
};

} // namespace di::persona_engine::worker_pool
