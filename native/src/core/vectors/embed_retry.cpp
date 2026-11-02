#include "core/vectors/embed_retry.hpp"

#include "base/json.hpp"
#include "base/log.hpp"
#include "base/strings.hpp"

namespace di::core::vectors {

EmbeddingHttpError::EmbeddingHttpError(int status, const std::string& status_text)
    : std::runtime_error(str::trim("embedding HTTP " + std::to_string(status) + " " + status_text)), status_(status) {}

bool is_retryable_embedding_error(const std::exception& err) {
    if (const auto* http = dynamic_cast<const EmbeddingHttpError*>(&err)) return http->status() >= 500 || http->status() == 429;
    return dynamic_cast<const EmbeddingTransportError*>(&err) != nullptr;
}

void default_embed_retry_warn(const std::string& message) { log::warn("[embed-retry] " + message); }

void default_embed_retry_sleep(double ms) {
    if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<long long>(ms)));
}

void validate_embed_retry_options(const EmbedRetryOptions& options) {
    if (!std::isfinite(options.attempts) || std::floor(options.attempts) != options.attempts || options.attempts < 1)
        throw std::invalid_argument("embedding retry attempts must be a positive integer");
    if (!std::isfinite(options.baseDelayMs) || options.baseDelayMs < 0)
        throw std::invalid_argument("embedding retry baseDelayMs must be a non-negative finite number");
}

std::string embed_retry_warn_message(const std::string& error_message, double delay_ms, int attempt, int attempts) {
    return "一過性の埋め込み失敗 (" + error_message + ") — " + js_number_to_string(delay_ms) + "ms 待って再試行 (" +
           std::to_string(attempt) + "/" + std::to_string(attempts - 1) + ")";
}

} // namespace di::core::vectors
