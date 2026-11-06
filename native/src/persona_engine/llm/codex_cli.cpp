#include "persona_engine/llm/codex_cli.hpp"

#include <chrono>
#include <filesystem>
#include <system_error>

#include "base/crypto.hpp"
#include "base/env.hpp"
#include "base/fsutil.hpp"
#include "base/strings.hpp"
#include "lib/one_shot/one_shot.hpp"
#include "persona_engine/llm/llm_vg.hpp"
#include "persona_engine/llm/model_spec.hpp"

namespace di::persona_engine {

namespace {
const char* const CODEX_ENV_ALLOWLIST[] = {"PATH",     "Path",         "PATHEXT",    "SystemRoot", "WINDIR",
                                           "COMSPEC",  "TEMP",         "TMP",        "HOME",       "USERPROFILE",
                                           "APPDATA",  "LOCALAPPDATA", "CODEX_HOME", "LANG",       "LC_ALL"};

std::string compose(const std::optional<std::string>& system, const std::string& user) {
    if (!system || system->empty()) return user;
    return "[system]\n" + *system + "\n\n[user]\n" + user;
}

/** mkdtemp(tmpdir()/di-codex-XXXXXX) 相当。作れなければ nullopt。 */
std::optional<std::filesystem::path> make_work_dir() {
    std::error_code ec;
    const auto base = std::filesystem::temp_directory_path(ec);
    if (ec) return std::nullopt;
    for (int attempt = 0; attempt < 8; ++attempt) {
        const auto dir = base / ("di-codex-" + crypto::hex_encode(crypto::random_bytes(6)));
        if (std::filesystem::create_directory(dir, ec) && !ec) {
            std::filesystem::permissions(dir, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, ec);
            return dir;
        }
    }
    return std::nullopt;
}
} // namespace

std::map<std::string, std::string> build_codex_environment(const std::map<std::string, std::string>& source) {
    std::map<std::string, std::string> env;
    for (const char* key : CODEX_ENV_ALLOWLIST) {
        auto it = source.find(key);
        if (it != source.end()) env[key] = it->second;
    }
    return env;
}

std::vector<std::string> codex_cli_args(const std::string& model, const std::optional<std::string>& effort,
                                        const std::string& out_file) {
    std::vector<std::string> args = {"exec", "-m", model};
    if (effort) args.insert(args.end(), {"-c", "model_reasoning_effort=\"" + *effort + "\""});
    args.insert(args.end(), {"-s", "read-only", "--ephemeral", "--skip-git-repo-check", "--color", "never", "-o", out_file, "-"});
    return args;
}

CodexCliClient::CodexCliClient(CodexCliClientOptions options)
    : cli_path_(options.cliPath.value_or("codex")),
      default_model_(std::move(options.defaultModel)),
      default_timeout_ms_(options.defaultTimeoutMs.value_or(CODEX_DEFAULT_TIMEOUT_MS)),
      cwd_(std::move(options.cwd)) {}

LLMResult CodexCliClient::invoke(const LLMInvokeArgs& args) {
    if (args.conversationOnly) return LLMResult::failure("Codex CLI conversation-only capability is not configured");
    const ModelSpec spec = parse_model_spec(args.model ? args.model : default_model_);
    if (auto err = validate_model_spec(spec, ModelProvider::codex)) return LLMResult::failure("codex-cli: " + *err);
    const long long timeout_ms = args.timeoutMs.value_or(default_timeout_ms_);
    const std::string prompt = compose(args.system, args.prompt);

    const auto work_dir = make_work_dir();
    if (!work_dir) return LLMResult::failure("codex-cli: 一時領域を作成できません");
    const std::string out_file = fs::from_path(*work_dir / "last-message.txt");
    const auto cli_args = codex_cli_args(spec.model, spec.effort, out_file);

    LLMResult result;
    {
        proc::Result run;
        bool spawn_failed = false;
        try {
            lib::one_shot::RunOptions options;
            options.env = build_codex_environment(env::snapshot());
            options.cwd = cwd_.value_or("");
            options.stdin_data = prompt;
            options.timeout = std::chrono::milliseconds(timeout_ms);
            // イベント出力 (stdout) と診断出力 (stderr) は捨てる。溜める量にも上限を置く。
            options.max_stdout = 32u << 20;
            options.max_stderr = 4u << 20;
            run = lib::one_shot::run(cli_path_, cli_args, options);
        } catch (const std::exception&) {
            spawn_failed = true;
        }
        if (spawn_failed) {
            result = LLMResult::failure("codex cli spawn failed");
        } else if (!run.started) {
            // Node の 'error' イベント (実行ファイルが無い等)。ローカルパスは上位へ出さない。
            const bool missing = str::starts_with(run.start_error, "executable not found");
            result = LLMResult::failure(std::string("codex cli error") + (missing ? " (ENOENT)" : ""));
        } else if (run.timed_out) {
            result = LLMResult::failure("codex cli timeout (" + std::to_string(timeout_ms) + "ms)");
        } else if (run.output_exceeded) {
            result = LLMResult::failure("codex cli: output exceeds limit");
        } else if (run.exit_code != 0) {
            result = LLMResult::failure("codex cli exit " + (run.exit_code < 0 ? std::string("null") : std::to_string(run.exit_code)));
        } else {
            std::string text;
            const auto size = fs::file_size(out_file);
            if (size && *size > CODEX_MAX_RESPONSE_BYTES) {
                result = LLMResult::failure("codex cli: response exceeds " + std::to_string(CODEX_MAX_RESPONSE_BYTES) + " bytes");
            } else {
                if (auto content = fs::read_file(out_file, CODEX_MAX_RESPONSE_BYTES)) text = str::trim(*content);
                result = text.empty() ? LLMResult::failure("codex cli: empty response") : LLMResult::success(text);
            }
        }
    }

    LlmLogArgs log;
    log.backend = "codex-cli";
    log.model = spec.effort ? spec.model + "@" + *spec.effort : spec.model;
    log.system = args.system;
    log.prompt = args.prompt;
    log.ok = result.ok;
    if (!result.ok) log.error = result.error;
    log_llm(log);

    std::error_code ec;
    std::filesystem::remove_all(*work_dir, ec);  // best-effort cleanup
    return result;
}

} // namespace di::persona_engine
