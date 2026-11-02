#pragma once
// ローカル LLM (OpenAI 互換 `/v1/chat/completions`) 実装 (src/persona-engine/llm/local-openai.ts)。
// Ollama / vLLM / LM Studio / llama.cpp server に baseUrl + model を差すだけで繋がる。
// baseUrl は運用者が設定したローカルのサービスなので allow_private_network = true で呼ぶ。
// 応答は上限つきで読む (本文 8 MiB・JSON 要素数・深さ)。apiKey はログに出さない。
#include <cstddef>
#include <functional>
#include <optional>
#include <string>

#include "infra/http_client.hpp"
#include "persona_engine/llm/client.hpp"

namespace di::persona_engine {

struct LocalOpenAiClientOptions {
    /** OpenAI 互換エンドポイントのベース URL (末尾 `/v1` まで)。 */
    std::string baseUrl;
    /** 既定モデル (例 `gemma4:12b`)。invoke args.model が優先。 */
    std::optional<std::string> defaultModel;
    /** 任意。設定時のみ Authorization: Bearer を送る。 */
    std::optional<std::string> apiKey;
    std::optional<long long> defaultTimeoutMs;
    /** テスト用 fetch 差し替え。 */
    std::function<http::ClientResponse(const http::ClientRequest&)> fetchImpl;
};

inline constexpr int LOCAL_OPENAI_DEFAULT_MAX_TOKENS = 4096;
inline constexpr long long LOCAL_OPENAI_DEFAULT_TIMEOUT_MS = 120'000;
inline constexpr std::size_t LOCAL_OPENAI_MAX_RESPONSE_BYTES = 8u << 20;

class LocalOpenAiClient : public LLMClient {
public:
    explicit LocalOpenAiClient(LocalOpenAiClientOptions options);
    LLMResult invoke(const LLMInvokeArgs& args) override;
    const std::string& base_url() const { return base_url_; }

private:
    std::string base_url_;
    std::string default_model_;
    std::string api_key_;
    long long default_timeout_ms_;
    std::function<http::ClientResponse(const http::ClientRequest&)> fetch_;
};

} // namespace di::persona_engine
