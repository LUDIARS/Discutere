#pragma once
// 埋め込みリクエストのリトライ判定と再試行 (src/core/vectors/embed-retry.ts)。
//   - 一過性 (タイムアウト / ネットワーク断 / 5xx / 429) → 指数バックオフで再試行
//   - 恒常 (4xx / 応答形式不正) → 即 throw (fail-fast、リトライで隠さない)
// リトライ発生は warn で必ず観測可能にする (silent retry 禁止)。
#include <chrono>
#include <cmath>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>

namespace di::core::vectors {

/** HTTP ステータスを保持する埋め込みエラー (リトライ判定に使う)。応答本文は message に載せない。 */
class EmbeddingHttpError : public std::runtime_error {
public:
    EmbeddingHttpError(int status, const std::string& status_text);
    int status() const { return status_; }

private:
    int status_;
};

/** 通信の失敗 (TS の AbortError / TimeoutError / fetch の TypeError に当たる一過性のエラー)。 */
class EmbeddingTransportError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/** 一過性 (再試行に値する) エラーか。 */
bool is_retryable_embedding_error(const std::exception& err);

struct EmbedRetryOptions {
    /** 総試行回数 (1 = リトライ無し)。 */
    double attempts = 3;
    /** 初回リトライ前の待ち ms。以降は 2 倍ずつ。 */
    double baseDelayMs = 2'000;
};

inline constexpr EmbedRetryOptions DEFAULT_EMBED_RETRY{3, 2'000};

using WarnFn = std::function<void(const std::string&)>;
using SleepFn = std::function<void(double ms)>;

/** 既定の warn ("[embed-retry] ..." を log::warn)。 */
void default_embed_retry_warn(const std::string& message);
/** 既定の sleep (呼んだスレッドで待つ)。 */
void default_embed_retry_sleep(double ms);

/** 設定検査 (TS と同じ文言で std::invalid_argument)。 */
void validate_embed_retry_options(const EmbedRetryOptions& options);

/** 再試行の warn 文言 (TS と同じ)。 */
std::string embed_retry_warn_message(const std::string& error_message, double delay_ms, int attempt, int attempts);

/**
 * 一過性エラーに限り指数バックオフで再試行する。
 * 恒常エラーは即 throw、試行し尽くしたら最後のエラーを throw する。
 */
template <class F>
auto with_embed_retry(F&& run, const EmbedRetryOptions& options = DEFAULT_EMBED_RETRY,
                      const WarnFn& warn = default_embed_retry_warn, const SleepFn& sleep = default_embed_retry_sleep)
    -> decltype(run()) {
    validate_embed_retry_options(options);
    // 1e9 回を超える指定は実質無限なので int に収める
    const int attempts = options.attempts > 1e9 ? 1'000'000'000 : static_cast<int>(options.attempts);
    for (int attempt = 1;; ++attempt) {
        try {
            return run();
        } catch (const std::exception& err) {
            if (!is_retryable_embedding_error(err) || attempt >= attempts) throw;
            const double delay = options.baseDelayMs * std::pow(2.0, attempt - 1);
            warn(embed_retry_warn_message(err.what(), delay, attempt, attempts));
            sleep(delay);
        }
    }
}

} // namespace di::core::vectors
