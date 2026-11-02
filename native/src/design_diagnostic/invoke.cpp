#include "design_diagnostic/invoke.hpp"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#include "design_diagnostic/contracts.hpp"

namespace di::design_diagnostic {

namespace {

std::atomic<int> g_running{0};

struct Shared {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    bool threw = false;
    persona_engine::LLMResult result;
};

} // namespace

std::string invoke_diagnostic(const DiagnosticPrompt& prompt, const DiagnosticDependencies& deps,
                              std::optional<std::chrono::milliseconds> timeout) {
    if (!deps.llm) throw DiagnosticError("llm_unavailable", "Diagnostic requires a configured LLM client");
    if (g_running.fetch_add(1) >= kMaxConcurrentDiagnostics) {
        g_running.fetch_sub(1);
        throw DiagnosticError("llm_unavailable", "Diagnostic is busy");
    }
    persona_engine::LLMInvokeArgs args;
    args.system = prompt.system;
    args.prompt = prompt.prompt;
    args.model = deps.model;
    args.maxTokens = 8'000;
    args.timeoutMs = DIAGNOSTIC_LIMITS.timeoutMs;
    args.conversationOnly = true;

    auto shared = std::make_shared<Shared>();
    auto llm = deps.llm;
    try {
        std::thread([shared, llm, args] {
            persona_engine::LLMResult r;
            bool threw = false;
            try {
                r = llm->invoke(args);
            } catch (...) {
                threw = true;
            }
            {
                std::lock_guard lock(shared->mutex);
                shared->result = std::move(r);
                shared->threw = threw;
                shared->done = true;
            }
            shared->cv.notify_all();
            g_running.fetch_sub(1);
        }).detach();
    } catch (...) {
        g_running.fetch_sub(1);
        throw DiagnosticError("llm_failed", "Diagnostic LLM invocation failed");
    }

    std::unique_lock lock(shared->mutex);
    const auto limit = timeout.value_or(std::chrono::milliseconds(DIAGNOSTIC_LIMITS.timeoutMs));
    if (!shared->cv.wait_for(lock, limit, [&] { return shared->done; }))
        throw DiagnosticError("llm_timeout", "Diagnostic timed out");
    if (shared->threw) throw DiagnosticError("llm_failed", "Diagnostic LLM invocation failed");
    if (!shared->result.ok) throw DiagnosticError("llm_failed", "Diagnostic LLM returned failure");
    return shared->result.text;
}

} // namespace di::design_diagnostic
