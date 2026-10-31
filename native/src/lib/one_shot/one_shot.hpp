#pragma once
// @ludiars/one-shot の移植 (lib/lapilli/packages/one-shot/src)。
// claude print mode (-p) / codex exec を 1 回だけ起動する境界。シェルを経由しない (引数配列)。
// API 課金を防ぐため API キー系・親セッション連携の環境変数を子に渡さない。再試行しない。
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "base/process.hpp"

namespace di::lib::one_shot {

/** 役割名 → 既定モデル (Concordia の 2026-10-03 時点の写し)。 */
const std::map<std::string, std::string>& default_models();

/** 役割名 (opus/sonnet/haiku/...) か ID を解決する。不正は std::invalid_argument。 */
std::string resolve_model(const std::optional<std::string>& role_or_id, const std::string& provider,
                          const std::map<std::string, std::string>& env);
std::vector<std::string> resolve_model_args(const std::vector<std::string>& args, const std::string& provider,
                                            const std::map<std::string, std::string>& env);
/** ANTHROPIC_API_KEY 等を除いた環境。 */
std::map<std::string, std::string> subscription_environment(const std::map<std::string, std::string>& source);

struct Launch {
    std::string file;
    std::vector<std::string> args;
    std::map<std::string, std::string> env;
    std::string cwd;
};

/** prepareOneShot。claude -p / codex exec 以外は std::invalid_argument。 */
Launch prepare(const std::string& file, const std::vector<std::string>& args,
               const std::optional<std::map<std::string, std::string>>& env = std::nullopt, const std::string& cwd = {});

struct RunOptions {
    std::optional<std::map<std::string, std::string>> env;
    std::string cwd;
    std::string stdin_data;
    std::chrono::milliseconds timeout{120'000};
    std::size_t max_stdout = 16u << 20;
    std::size_t max_stderr = 1u << 20;
    std::function<bool()> should_cancel;
};

/** prepare して起動し、終わるまで待つ。 */
proc::Result run(const std::string& file, const std::vector<std::string>& args, const RunOptions& options);

} // namespace di::lib::one_shot
