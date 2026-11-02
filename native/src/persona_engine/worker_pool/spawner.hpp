#pragma once
// 常駐ワーカーの spawn (src/persona-engine/worker-pool/spawner.ts)。
// `lictor <bin> --model <model> [--permission-mode acceptEdits]` を引数配列で起動する (シェルを経由しない)。
// 親から切り離した常駐プロセス (POSIX: 新しいセッション / Windows: DETACHED_PROCESS) にし、標準入出力は捨てる。
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "persona_engine/worker_pool/types.hpp"

namespace di::persona_engine::worker_pool {

/** git-bash パス解決 (config 明示 → 既知候補の自動検出)。 */
std::optional<std::string> resolve_git_bash(const std::optional<std::string>& explicit_path);

struct SpawnedWorker {
    std::optional<std::int64_t> pid;
};

struct SpawnWorkerArgs {
    WorkerConfig worker;
    std::string promptPath;
    /** ワーカーの cwd (= worker-home)。 */
    std::string cwd;
    WorkerPoolConfig cfg;
};

/** lictor に渡す引数 (lictor 以降)。テスト用に公開。 */
std::vector<std::string> lictor_args(const WorkerConfig& worker);
/** 子に渡す環境変数 (親の環境 + ワーカー用の値、親の Lictor/Concordia 連携は剥がす)。テスト用に公開。 */
std::map<std::string, std::string> build_worker_environment(const std::map<std::string, std::string>& parent,
                                                            const SpawnWorkerArgs& args);

/** 1 ワーカーを spawn し、子プロセス pid を返す (起動できなければ pid=nullopt)。 */
SpawnedWorker spawn_worker(const SpawnWorkerArgs& args);

/** spawn した子プロセスツリーを kill する (best-effort)。lictorPid を優先、cmdPid はフォールバック。 */
void kill_worker(std::optional<std::int64_t> cmd_pid, std::optional<std::int64_t> lictor_pid = std::nullopt);

struct DetachedSpawn {
    std::vector<std::string> argv;
    std::map<std::string, std::string> env;
    std::string cwd;
};
/** 切り離した常駐プロセスを起動する (stdio は捨てる。終了は背景で回収する)。テスト用に公開。 */
std::optional<std::int64_t> spawn_detached(const DetachedSpawn& spec, std::string* error = nullptr);

} // namespace di::persona_engine::worker_pool
