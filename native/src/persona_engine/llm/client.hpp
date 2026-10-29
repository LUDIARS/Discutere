#pragma once
// LLMClient — persona-engine / flow が LLM を呼ぶ唯一の境界 (src/persona-engine/llm/client.ts)。
// Promise の代わりに同期 API。呼んだスレッドで応答を待つ (claude -p は 1 回 8〜11 秒かかる)。
#include <memory>
#include <optional>
#include <string>

namespace di::persona_engine {

/** src/persona-engine/types.ts の TokenUsage。 */
struct TokenUsage {
    std::optional<double> input_tokens;
    std::optional<double> output_tokens;
    std::optional<double> cache_read_input_tokens;
    std::optional<double> cache_creation_input_tokens;
    std::optional<double> cost_usd;
    std::optional<std::string> model;
};

struct LLMInvokeArgs {
    /** 外部の会話にエージェントの道具や常駐ワーカーの権限を与えない。 */
    bool conversationOnly = false;
    std::string prompt;
    std::optional<std::string> system;
    std::optional<std::string> model;
    std::optional<int> maxTokens;
    /** タイムアウト ms (未指定は各クライアントの既定)。 */
    std::optional<long long> timeoutMs;
    /** 発話するペルソナ id (worker-pool のルーティング用)。 */
    std::optional<std::string> personaId;
};

/** { ok: true, text, usage } | { ok: false, error } */
struct LLMResult {
    bool ok = false;
    std::string text;
    std::optional<TokenUsage> usage;
    std::string error;

    static LLMResult success(std::string text, std::optional<TokenUsage> usage = std::nullopt) {
        LLMResult r;
        r.ok = true;
        r.text = std::move(text);
        r.usage = std::move(usage);
        return r;
    }
    static LLMResult failure(std::string error) {
        LLMResult r;
        r.error = std::move(error);
        return r;
    }
};

class LLMClient {
public:
    virtual ~LLMClient() = default;
    virtual LLMResult invoke(const LLMInvokeArgs& args) = 0;
};

using LLMClientPtr = std::shared_ptr<LLMClient>;

} // namespace di::persona_engine
