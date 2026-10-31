#include "persona_engine/llm/claude_cli.hpp"

#include <chrono>

#include "base/env.hpp"
#include "base/fsutil.hpp"
#include "base/json.hpp"
#include "base/strings.hpp"
#include "base/time.hpp"
#include "lib/one_shot/one_shot.hpp"
#include "persona_engine/llm/concordia_cost.hpp"
#include "persona_engine/llm/llm_vg.hpp"
#include "persona_engine/llm/model_spec.hpp"

namespace di::persona_engine {

namespace {
constexpr long long kDefaultTimeoutMs = 120'000;
}

ClaudeCliClient::ClaudeCliClient(ClaudeCliClientOptions options)
    : cli_path_(options.cliPath.value_or("claude")),
      default_timeout_ms_(options.defaultTimeoutMs.value_or(kDefaultTimeoutMs)),
      git_bash_path_(std::move(options.gitBashPath)),
      default_model_(std::move(options.defaultModel)) {}

std::string compose_prompt(const std::optional<std::string>& system, const std::string& user) {
    if (!system || system->empty()) return user;
    return "[system]\n" + *system + "\n\n[user]\n" + user;
}

std::vector<std::string> claude_cli_args(const std::optional<std::string>& model, bool conversation_only) {
    const ModelSpec spec = parse_model_spec(model);
    std::vector<std::string> args = {"-p", "--output-format", "json"};
    if (conversation_only) {
        args.insert(args.end(), {"--tools=", "--strict-mcp-config", "--disable-slash-commands", "--safe-mode"});
    }
    if (!spec.model.empty()) args.insert(args.end(), {"--model", spec.model});
    if (spec.effort) args.insert(args.end(), {"--effort", *spec.effort});
    return args;
}

LLMResult parse_claude_cli_result(const std::string& raw) {
    std::string error;
    JsonLimits limits;
    limits.max_bytes = 16u << 20;
    auto parsed = parse_json(raw, limits, &error);
    if (!parsed) return LLMResult::failure("claude cli json parse failed: " + error + ": " + str::js_slice(raw, 0, 200));
    const Json* env = nullptr;
    if (parsed->is_array()) {
        for (const auto& e : *parsed) {
            if (e.is_object() && json_string(e, "type") == "result") {
                env = &e;
                break;
            }
        }
        if (!env && !parsed->empty()) env = &parsed->back();
    } else if (parsed->is_object()) {
        env = &*parsed;
    }
    if (!env || !env->is_object()) return LLMResult::failure("claude cli json: no result envelope");
    const auto subtype = json_opt_string(*env, "subtype");
    if (json_bool(*env, "is_error") || (subtype && !subtype->empty() && *subtype != "success")) {
        return LLMResult::failure("claude cli result error (" + subtype.value_or("unknown") +
                                  "): " + str::js_slice(json_string(*env, "result"), 0, 200));
    }
    const auto result = json_opt_string(*env, "result");
    const std::string text = result ? str::trim(*result) : std::string();
    if (text.empty()) return LLMResult::failure("claude cli json: empty result text");
    const Json& u = json_object(*env, "usage");
    TokenUsage usage;
    usage.input_tokens = json_opt_number(u, "input_tokens");
    usage.output_tokens = json_opt_number(u, "output_tokens");
    usage.cache_read_input_tokens = json_opt_number(u, "cache_read_input_tokens");
    usage.cache_creation_input_tokens = json_opt_number(u, "cache_creation_input_tokens");
    usage.cost_usd = json_opt_number(*env, "total_cost_usd");
    return LLMResult::success(text, usage);
}

LLMResult ClaudeCliClient::invoke(const LLMInvokeArgs& args) {
    const long long timeout_ms = args.timeoutMs.value_or(default_timeout_ms_);
    const std::optional<std::string> model = args.model ? args.model : default_model_;
    const std::string prompt = compose_prompt(args.system, args.prompt);
    if (model && !model->empty()) {
        if (auto err = validate_model_spec(parse_model_spec(model), ModelProvider::claude))
            return LLMResult::failure("claude cli: " + *err);
    }
    const auto cli_args = claude_cli_args(model, args.conversationOnly);
    auto env = env::snapshot();
    if (git_bash_path_) env["CLAUDE_CODE_GIT_BASH_PATH"] = *git_bash_path_;
#ifdef _WIN32
    if (env.find("CLAUDE_CODE_GIT_BASH_PATH") == env.end()) {
        for (const char* c : {"C:\\Program Files\\Git\\bin\\bash.exe", "C:\\Program Files (x86)\\Git\\bin\\bash.exe"}) {
            if (fs::is_file(c)) {
                env["CLAUDE_CODE_GIT_BASH_PATH"] = c;
                break;
            }
        }
    }
#endif
    // Lictor / Concordia 連携を持ち込まない。
    env.erase("CONCORDIA_HOOK");
    env.erase("LICTOR_PORT");

    const std::string command = cli_path_ + " " + str::join(cli_args, " ");
    const auto started = time::steady_ms();
    proc::Result run;
    try {
        lib::one_shot::RunOptions options;
        options.env = env;
        options.cwd = env::cwd();
        options.stdin_data = prompt;
        options.timeout = std::chrono::milliseconds(timeout_ms);
        run = lib::one_shot::run(cli_path_, cli_args, options);
    } catch (const std::exception& e) {
        report_concordia_cost_one_shot({"discutere", "claude", command, model, env::cwd(), prompt, "error", -1,
                                        time::steady_ms() - started, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
                                        Json{{"error", e.what()}}});
        return LLMResult::failure(std::string("claude cli spawn failed: ") + e.what());
    }
    if (!run.started) {
        report_concordia_cost_one_shot({"discutere", "claude", command, model, env::cwd(), prompt, "error", -1,
                                        time::steady_ms() - started, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
                                        Json{{"error", run.start_error}}});
        return LLMResult::failure("claude cli spawn failed: " + run.start_error);
    }
    if (run.timed_out) {
        report_concordia_cost_one_shot({"discutere", "claude", command, model, env::cwd(), prompt, "timeout", -1,
                                        time::steady_ms() - started, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
                                        std::nullopt});
        return LLMResult::failure("claude cli timeout (" + std::to_string(timeout_ms) + " ms)");
    }
    LLMResult result;
    if (run.output_exceeded) {
        result = LLMResult::failure("claude cli output too large");
    } else if (run.exit_code != 0) {
        result = LLMResult::failure("claude cli exit " + std::to_string(run.exit_code) + ": " + str::js_slice(run.err, 0, 200));
    } else {
        const std::string raw = str::trim(run.out);
        result = raw.empty() ? LLMResult::failure("claude cli empty output") : parse_claude_cli_result(raw);
    }
    LlmLogArgs log_args;
    log_args.backend = "claude-cli";
    log_args.model = model.value_or("(default)");
    log_args.prompt = prompt;
    log_args.ok = result.ok;
    if (result.ok && result.usage) {
        log_args.input_tokens = result.usage->input_tokens;
        log_args.output_tokens = result.usage->output_tokens;
        log_args.cache_read_tokens = result.usage->cache_read_input_tokens;
        log_args.cache_creation_tokens = result.usage->cache_creation_input_tokens;
        log_args.cost_usd = result.usage->cost_usd;
    } else if (!result.ok) {
        log_args.error = result.error;
    }
    log_llm(log_args);
    ConcordiaCostOneShot cost{"discutere", "claude", command, model, env::cwd(), prompt, result.ok ? "ok" : "error",
                              run.exit_code, time::steady_ms() - started, std::nullopt, std::nullopt, std::nullopt,
                              std::nullopt, std::nullopt};
    if (result.ok && result.usage) {
        cost.input_tokens = result.usage->input_tokens;
        cost.output_tokens = result.usage->output_tokens;
        cost.total_tokens = result.usage->input_tokens.value_or(0) + result.usage->output_tokens.value_or(0);
        cost.cost_usd = result.usage->cost_usd;
        Json meta = Json::object();
        if (result.usage->cache_read_input_tokens) meta["cacheReadTokens"] = *result.usage->cache_read_input_tokens;
        if (result.usage->cache_creation_input_tokens) meta["cacheCreationTokens"] = *result.usage->cache_creation_input_tokens;
        cost.metadata = meta;
    } else if (!result.ok) {
        cost.metadata = Json{{"error", result.error}, {"stderr", str::js_slice(run.err, 0, 1000)}};
    }
    report_concordia_cost_one_shot(std::move(cost));
    return result;
}

} // namespace di::persona_engine
