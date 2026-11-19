#pragma once
// Lapilli の one-shot 起動準備を、Discutere のプロセス実行とキャンセル契約に接続する。
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "base/process.hpp"

namespace di::lib::one_shot {

struct Launch {
    std::string file;
    std::vector<std::string> args;
    std::map<std::string, std::string> env;
    std::string cwd;
};

/** シェルを介さない実行ファイル・引数・環境を Lapilli で準備する。 */
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
