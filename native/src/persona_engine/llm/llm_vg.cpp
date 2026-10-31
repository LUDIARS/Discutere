#include "persona_engine/llm/llm_vg.hpp"

#include <mutex>

#include "base/env.hpp"
#include "base/fsutil.hpp"
#include "base/json.hpp"
#include "base/strings.hpp"
#include "base/time.hpp"

namespace di::persona_engine {

void log_llm(const LlmLogArgs& args) {
    try {
        Json ctx = Json::object();
        ctx["backend"] = args.backend;
        ctx["model"] = args.model;
        ctx["prompt_chars"] = str::js_length(args.prompt);
        ctx["prompt"] = args.prompt;
        ctx["ok"] = args.ok;
        if (args.kind) ctx["kind"] = *args.kind;
        if (args.system) {
            ctx["system_chars"] = str::js_length(*args.system);
            ctx["system"] = *args.system;
        }
        if (args.input_tokens) ctx["input_tokens"] = *args.input_tokens;
        if (args.output_tokens) ctx["output_tokens"] = *args.output_tokens;
        if (args.cache_read_tokens) ctx["cache_read_tokens"] = *args.cache_read_tokens;
        if (args.cache_creation_tokens) ctx["cache_creation_tokens"] = *args.cache_creation_tokens;
        if (args.cost_usd) ctx["cost_usd"] = *args.cost_usd;
        if (args.error) ctx["error"] = *args.error;
        const auto now = time::now_ms();
        Json rec = Json::object();
        rec["ts"] = now;
        rec["level"] = args.ok ? "info" : "warn";
        rec["service"] = "discutere";
        rec["channel"] = "llm";
        rec["msg"] = "[llm] " + args.backend + " " + args.model;
        rec["pid"] = env::pid();
        rec["ctx"] = std::move(ctx);
        const std::string root = env::get("VESTIGIUM_LOGS_DIR").value_or(fs::join(env::cwd(), "logs"));
        static std::mutex m;
        std::lock_guard lock(m);
        fs::append_file(fs::join(fs::join(root, "discutere"), time::ymd_utc(now) + ".jsonl"), json_stringify(rec) + "\n");
    } catch (...) {
        // failure-safe
    }
}

} // namespace di::persona_engine
