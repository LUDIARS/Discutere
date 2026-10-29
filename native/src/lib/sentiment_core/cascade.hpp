#pragma once
// @ludiars/sentiment-core cascade.ts の移植: 0 段 (語彙) → 1 段 (ローカル LLM) → 2 段 (主 LLM) の感情判定。
#include <memory>
#include <optional>
#include <string>

#include "base/json.hpp"

namespace di::lib::sentiment_core {

/** "positive" | "negative" | "neutral" */
using Polarity = std::string;

struct SentimentResult {
    Polarity polarity;
    double score = 0;
    double confidence = 0;
    int tier = 0;  // 0 | 1 | 2
};

struct Signal {
    std::optional<double> upvotes;
    std::optional<bool> votedUp;
};

struct SentimentLlmRequest {
    std::string system;
    std::string prompt;
    int maxTokens = 0;
};

/** { ok: true, text } | { ok: false, error? } */
struct SentimentLlmResponse {
    bool ok = false;
    std::string text;
    std::string error;
};

class SentimentLlmClient {
public:
    virtual ~SentimentLlmClient() = default;
    /** 同期で応答を待つ。 */
    virtual SentimentLlmResponse invoke(const SentimentLlmRequest& request) = 0;
};

struct CascadeClients {
    std::shared_ptr<SentimentLlmClient> local;
    std::shared_ptr<SentimentLlmClient> main;
};

SentimentResult cascade_sentiment(std::string_view text, const std::optional<Signal>& signal, const CascadeClients& clients);

/** LLM の応答 ({"polarity":..,"confidence":..} を含む文字列) を読む。読めなければ nullopt (テスト用に公開)。 */
struct ParsedSentiment {
    Polarity polarity;
    double confidence = 0;
};
std::optional<ParsedSentiment> parse_sentiment_response(std::string_view raw);

/** LLM 応答 JSON の上限 (maxTokens 64 の応答に対して十分な大きさ)。 */
inline constexpr std::size_t kMaxSentimentResponseBytes = 64 * 1024;

Json to_json(const SentimentResult& result);

} // namespace di::lib::sentiment_core
