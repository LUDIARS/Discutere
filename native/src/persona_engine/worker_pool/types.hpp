#pragma once
// 常駐ワーカープール — 型定義 (src/persona-engine/worker-pool/types.ts)。spec/feature/persistent-worker-pool.md 参照。
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "base/executor.hpp"

namespace di::persona_engine::worker_pool {

/** "claude" | "codex" */
using WorkerProvider = std::string;

/** 1 ワーカー = 1 ペルソナ = 1 常駐 Lictor セッション。 */
struct WorkerConfig {
    /** ワーカー / ペルソナ id (一意)。例 "con-opus" */
    std::string id;
    /** 役割ラベル。例 "否定派" */
    std::string role;
    /** Lictor がラップする CLI。 */
    WorkerProvider provider;
    /** CLI に --model で渡すモデル ID。 */
    std::string model;
};

/** WorkerPool の動作設定 (config.workerPool より構築)。 */
struct WorkerPoolConfig {
    bool enabled = false;
    /** 議論 workspace。既存 knowledge と隔離する想定 ("debate")。 */
    std::string workspace;
    /** ワーカーが register / utterance を返す Discutere の base URL。 */
    std::string callbackBaseUrl;
    /** 各ワーカー (lictor claude/codex) を起動する cwd (= worker-home)。 */
    std::string workerCwd;
    /** Windows で lictor → claude CLI を起動する際の git-bash パス (空なら自動検出)。 */
    std::optional<std::string> gitBashPath;
    /** auto-inject の TUI 待ち ms。 */
    std::int64_t injectDelayMs = 0;
    /** 1 ターンの発話生成タイムアウト ms。 */
    std::int64_t turnTimeoutMs = 0;
    /** spawn 後、ワーカーが自己 register するまでの待ち上限 ms。 */
    std::int64_t registerTimeoutMs = 0;
    /** register 受領後、dispatch を許可するまでの待ち ms。 */
    std::int64_t registerSettleMs = 0;
    /** ターン JSON を書き出すディレクトリ。 */
    std::string turnsDir;
    /** standing prompt md を書き出すディレクトリ。 */
    std::string promptsDir;
    /** ワーカー定義。 */
    std::vector<WorkerConfig> workers;
};

/** pool が握る 1 ワーカーの実行時状態。 */
struct WorkerRuntime {
    WorkerConfig config;
    /** spawn した子プロセスの pid。kill 用フォールバック。 */
    std::optional<std::int64_t> pid;
    /** register.mjs が env から読んだ Lictor 自身の node プロセス PID (Windows で優先して kill)。 */
    std::optional<std::int64_t> lictorPid;
    /** 自己 register で受領した Lictor sidecar port。未登録は nullopt。 */
    std::optional<std::int64_t> lictorPort;
    /** settle 完了時刻 (epoch ms)。nullopt = まだ settle 待ち。 */
    std::optional<std::int64_t> readyAt;
    /** readyAt を設定するタイマー (global_scheduler の id)。stop でキャンセルする。 */
    std::optional<Scheduler::Id> settleTimer;
    /** 直近ターンが処理中か (二重 dispatch 防止)。 */
    bool busy = false;
    /** standing prompt md path。 */
    std::string promptPath;
};

} // namespace di::persona_engine::worker_pool
