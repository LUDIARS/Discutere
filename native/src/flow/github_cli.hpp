#pragma once
// src/flow/github-cli.ts の移植: GitHub CLI (gh) の認証状態とトークンを取る。
// gh は引数配列で起動する (シェルを経由しない)。期限 5 秒・出力上限つき。トークンはログに出さない。
#include <optional>
#include <string>
#include <vector>

namespace di::flow {

struct GhCommandResult {
    int exitCode = 0;
    std::string stdout_text;
    std::string stderr_text;
    std::optional<std::string> error;
};

struct GitHubCliAuthStatus {
    bool available = false;
    bool authenticated = false;
    std::string output;
};

/** gh の出力の上限 (execFile の既定 maxBuffer と同じ 1MiB)。 */
inline constexpr std::size_t kGhMaxOutputBytes = 1u << 20;

/** gh を起動する (executable はテスト用に差し替えられる。既定 "gh")。 */
GhCommandResult run_gh(const std::vector<std::string>& args, int timeout_ms = 5000, const std::string& executable = "gh");

/** ANSI の色指定 (ESC [ … m) を落とす。 */
std::string strip_ansi(const std::string& s);
/** gh のトークン (ghp_ / gho_ / ghu_ / ghs_ / ghr_ / github_pat_ + 20 文字以上) を伏せる。 */
std::string redact_secrets(const std::string& s);

GitHubCliAuthStatus get_github_cli_auth_status(const std::string& executable = "gh");
/** 認証済みならトークン、そうでなければ nullopt。 */
std::optional<std::string> get_github_cli_token(const std::string& executable = "gh");

} // namespace di::flow
