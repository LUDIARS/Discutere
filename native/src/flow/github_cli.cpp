#include "flow/github_cli.hpp"

#include "base/process.hpp"
#include "base/strings.hpp"

namespace di::flow {

namespace {

bool is_word(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// \b<prefix>[A-Za-z0-9_]{20,}\b の長さ (一致しなければ 0)
std::size_t token_length_at(const std::string& s, std::size_t i, std::string_view prefix) {
    if (i > 0 && is_word(s[i - 1])) return 0;
    if (s.compare(i, prefix.size(), prefix) != 0) return 0;
    std::size_t j = i + prefix.size();
    while (j < s.size() && is_word(s[j])) ++j;
    return j - (i + prefix.size()) >= 20 ? j - i : 0;
}

} // namespace

GhCommandResult run_gh(const std::vector<std::string>& args, int timeout_ms, const std::string& executable) {
    proc::Options o;
    o.argv.push_back(executable);
    o.argv.insert(o.argv.end(), args.begin(), args.end());
    o.timeout = std::chrono::milliseconds(timeout_ms);
    o.max_stdout = kGhMaxOutputBytes;
    o.max_stderr = kGhMaxOutputBytes;
    const auto r = proc::run(o);  // 標準入力は空で閉じる (child.stdin.end())
    GhCommandResult out;
    out.stdout_text = r.out;
    out.stderr_text = r.err;
    if (!r.started) {
        // Node の execFile と同じ文言 (ENOENT 等)
        out.exitCode = 1;
        out.error = "spawn " + executable + " ENOENT" + (r.start_error.empty() ? "" : " (" + r.start_error + ")");
        return out;
    }
    const std::string command = "Command failed: " + str::join(o.argv, " ");
    if (r.timed_out || r.cancelled || r.output_exceeded || r.exit_code < 0) {
        out.exitCode = 1;  // 強制終了 (code は数値でない)
        out.error = command + (r.output_exceeded ? "\nmaxBuffer exceeded" : "\n" + r.err);
        return out;
    }
    out.exitCode = r.exit_code;
    if (r.exit_code != 0) out.error = command + "\n" + r.err;
    return out;
}

std::string strip_ansi(const std::string& s) {
    // s.replace(/\x1b\[[0-9;]*m/g, "")
    std::string out;
    std::size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '\x1b' && i + 1 < s.size() && s[i + 1] == '[') {
            std::size_t j = i + 2;
            while (j < s.size() && ((s[j] >= '0' && s[j] <= '9') || s[j] == ';')) ++j;
            if (j < s.size() && s[j] == 'm') {
                i = j + 1;
                continue;
            }
        }
        out.push_back(s[i++]);
    }
    return out;
}

std::string redact_secrets(const std::string& s) {
    std::string out;
    std::size_t i = 0;
    while (i < s.size()) {
        std::size_t len = 0;
        if (s[i] == 'g') {
            for (const char* p : {"ghp_", "gho_", "ghu_", "ghs_", "ghr_", "github_pat_"}) {
                len = token_length_at(s, i, p);
                if (len) break;
            }
        }
        if (len) {
            out += "[redacted-token]";
            i += len;
            continue;
        }
        out.push_back(s[i++]);
    }
    return out;
}

GitHubCliAuthStatus get_github_cli_auth_status(const std::string& executable) {
    const auto r = run_gh({"auth", "status", "--hostname", "github.com"}, 5000, executable);
    std::string output = str::trim(redact_secrets(strip_ansi(r.stdout_text + r.stderr_text)));
    if (output.empty()) output = r.error.value_or("gh auth status returned no output");
    bool missing = false;
    if (r.error) {
        const std::string lower = str::to_lower_ascii(*r.error);
        missing = lower.find("enoent") != std::string::npos || lower.find("not recognized") != std::string::npos ||
                  lower.find("not found") != std::string::npos;
    }
    return {!r.error || !missing, r.exitCode == 0, output};
}

std::optional<std::string> get_github_cli_token(const std::string& executable) {
    const auto r = run_gh({"auth", "token", "--hostname", "github.com"}, 5000, executable);
    if (r.exitCode != 0) return std::nullopt;
    const std::string token = str::trim(r.stdout_text);
    if (token.empty()) return std::nullopt;
    return token;
}

} // namespace di::flow
