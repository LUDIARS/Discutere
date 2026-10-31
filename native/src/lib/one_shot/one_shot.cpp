#include "lib/one_shot/one_shot.hpp"

#include <algorithm>

#include "base/env.hpp"
#include "base/fsutil.hpp"
#include "base/strings.hpp"

namespace di::lib::one_shot {

const std::map<std::string, std::string>& default_models() {
    static const std::map<std::string, std::string> models = {
        {"opus", "claude-opus-5-5"}, {"sonnet", "claude-sonnet-5"}, {"haiku", "claude-haiku-4-5-20251001"},
        {"fable", "claude-fable-5-1"}, {"astra", "gpt-6-astra"},    {"sol", "gpt-6-sol"},
        {"luna", "gpt-6-luna"},
    };
    return models;
}

namespace {
// /^[a-zA-Z0-9][a-zA-Z0-9._:/-]*$/
bool valid_model(const std::string& m) {
    if (m.empty()) return false;
    for (std::size_t i = 0; i < m.size(); ++i) {
        const char c = m[i];
        const bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (i == 0 ? !alnum : !(alnum || c == '.' || c == '_' || c == ':' || c == '/' || c == '-')) return false;
    }
    return true;
}

bool blocked_env(const std::string& key) {
    const std::string k = str::to_upper(key);
    static const char* exact[] = {"ANTHROPIC_API_KEY", "ANTHROPIC_AUTH_TOKEN", "ANTHROPIC_BASE_URL", "OPENAI_API_KEY",
                                  "OPENAI_BASE_URL", "CODEX_API_KEY", "CLAUDE_CODE_USE_BEDROCK", "CLAUDE_CODE_USE_VERTEX",
                                  "CLAUDE_CODE_USE_FOUNDRY", "CLAUDE_CODE_USE_ANTHROPIC_AWS", "CLAUDECODE", "CONCORDIA_HOOK"};
    for (const char* e : exact)
        if (k == e) return true;
    return str::starts_with(k, "LICTOR_");
}
} // namespace

std::string resolve_model(const std::optional<std::string>& role_or_id, const std::string& provider,
                          const std::map<std::string, std::string>& env) {
    const std::string requested = role_or_id && !role_or_id->empty() ? *role_or_id : (provider == "claude" ? "sonnet" : "sol");
    std::string model = requested;
    const auto& defaults = default_models();
    if (auto it = defaults.find(requested); it != defaults.end()) {
        auto ov = env.find("LUDIARS_ONESHOT_MODEL_" + str::to_upper(requested));
        model = (ov != env.end() && !ov->second.empty()) ? ov->second : it->second;
    }
    if (!valid_model(model)) throw std::invalid_argument("Invalid one-shot model");
    if (provider == "claude" && !str::starts_with(model, "claude-")) throw std::invalid_argument("Claude requires a Claude model");
    if (provider == "codex" && !str::starts_with(model, "gpt-")) throw std::invalid_argument("Codex requires a GPT model");
    return model;
}

std::vector<std::string> resolve_model_args(const std::vector<std::string>& args, const std::string& provider,
                                            const std::map<std::string, std::string>& env) {
    std::vector<std::string> result = args;
    auto index = std::find_if(result.begin(), result.end(), [](const auto& a) { return a == "--model" || a == "-m"; });
    auto inline_it = std::find_if(result.begin(), result.end(), [](const auto& a) { return str::starts_with(a, "--model="); });
    if (index != result.end()) {
        auto next = index + 1;
        if (next == result.end() || next->empty()) throw std::invalid_argument("Missing model argument");
        *next = resolve_model(*next, provider, env);
    } else if (inline_it != result.end()) {
        *inline_it = "--model=" + resolve_model(inline_it->substr(8), provider, env);
    } else {
        std::size_t position = 0;
        if (provider == "codex") {
            auto exec = std::find(result.begin(), result.end(), "exec");
            position = static_cast<std::size_t>(exec - result.begin()) + 1;
        }
        result.insert(result.begin() + static_cast<std::ptrdiff_t>(position),
                      {"--model", resolve_model(std::nullopt, provider, env)});
    }
    return result;
}

std::map<std::string, std::string> subscription_environment(const std::map<std::string, std::string>& source) {
    std::map<std::string, std::string> env;
    for (const auto& [k, v] : source)
        if (!blocked_env(k)) env.emplace(k, v);
    return env;
}

Launch prepare(const std::string& file_in, const std::vector<std::string>& args_in,
               const std::optional<std::map<std::string, std::string>>& env_in, const std::string& cwd) {
    std::string file = file_in;
    std::vector<std::string> argv = args_in;
    // 明示の cmd.exe ラッパーは、シェル文を実行せずに実体の CLI へ置き換える。
    const std::string base = str::to_lower_ascii(fs::basename(file));
    if (base == "cmd" || base == "cmd.exe") {
        auto marker = std::find_if(argv.begin(), argv.end(), [](const auto& a) { return str::to_lower_ascii(a) == "/c"; });
        if (marker == argv.end() || marker + 1 == argv.end() || (marker + 1)->find_first_of(" \t\n") != std::string::npos ||
            (marker + 1)->empty())
            throw std::invalid_argument("Unsupported CLI shell wrapper");
        file = *(marker + 1);
        argv.erase(argv.begin(), marker + 2);
    }
    const bool is_exec = std::find(argv.begin(), argv.end(), "exec") != argv.end();
    const bool is_print = std::find(argv.begin(), argv.end(), "-p") != argv.end() ||
                          std::find(argv.begin(), argv.end(), "--print") != argv.end();
    const std::string provider = is_exec ? "codex" : is_print ? "claude" : "";
    if (provider.empty()) throw std::invalid_argument("One-shot launch requires claude print mode or codex exec");
    auto env = subscription_environment(env_in ? *env_in : env::snapshot());
#ifdef _WIN32
    if (provider == "claude" && env.find("CLAUDE_CODE_GIT_BASH_PATH") == env.end()) {
        for (const char* c : {"C:/Program Files/Git/bin/bash.exe", "C:/Program Files (x86)/Git/bin/bash.exe"}) {
            if (fs::is_file(c)) {
                env["CLAUDE_CODE_GIT_BASH_PATH"] = c;
                break;
            }
        }
    }
#endif
    Launch launch;
    launch.cwd = cwd.empty() ? env::cwd() : cwd;
    std::vector<std::string> prefix;
#ifdef _WIN32
    // npm で入れた CLI (.cmd) は cmd.exe を経由させず、node + JS エントリを直接起動する。
    {
        std::string path;
        for (const auto& [k, v] : env)
            if (str::iequals_ascii(k, "PATH")) path = v;
        std::vector<std::string> dirs;
        if (file.find_first_of("/\\") != std::string::npos) dirs.push_back(fs::dirname(fs::resolve(fs::join(launch.cwd, file))));
        else dirs = str::split(path, ";");
        std::string name = fs::basename(file);
        const std::string lower = str::to_lower_ascii(name);
        if (str::ends_with(lower, ".cmd") || str::ends_with(lower, ".exe")) name = name.substr(0, name.size() - 4);
        bool resolved = false;
        for (const auto& dir : dirs) {
            if (dir.empty()) continue;
            const std::string native = fs::join(dir, name + ".exe");
            if (fs::is_file(native)) {
                file = native;
                resolved = true;
                break;
            }
            std::string entry;
            if (name == "claude") entry = fs::join(dir, "node_modules/@anthropic-ai/claude-code/cli.js");
            if (name == "codex") entry = fs::join(dir, "node_modules/@openai/codex/bin/codex.js");
            if (!entry.empty() && fs::is_file(entry)) {
                auto node = proc::find_executable("node", env);
                if (node) {
                    file = *node;
                    prefix.push_back(entry);
                    resolved = true;
                    break;
                }
            }
        }
        if (!resolved && (str::ends_with(lower, ".cmd") || str::ends_with(lower, ".bat")))
            throw std::invalid_argument("One-shot launch requires a native executable or a known npm CLI entry point");
    }
#endif
    launch.file = file;
    launch.args = prefix;
    for (auto& a : resolve_model_args(argv, provider, env)) launch.args.push_back(std::move(a));
    launch.env = std::move(env);
    return launch;
}

proc::Result run(const std::string& file, const std::vector<std::string>& args, const RunOptions& options) {
    const Launch launch = prepare(file, args, options.env, options.cwd);
    proc::Options p;
    p.argv.push_back(launch.file);
    for (const auto& a : launch.args) p.argv.push_back(a);
    p.env = launch.env;
    p.cwd = launch.cwd;
    p.stdin_data = options.stdin_data;
    p.timeout = options.timeout;
    p.max_stdout = options.max_stdout;
    p.max_stderr = options.max_stderr;
    p.should_cancel = options.should_cancel;
    return proc::run(p);
}

} // namespace di::lib::one_shot
