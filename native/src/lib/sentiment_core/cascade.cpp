#include "lib/sentiment_core/cascade.hpp"

#include <algorithm>
#include <cmath>

#include "base/strings.hpp"
#include "lib/sentiment_core/js_math.hpp"
#include "lib/sentiment_core/js_number.hpp"
#include "lib/sentiment_core/lexicon.hpp"

namespace di::lib::sentiment_core {

namespace {

constexpr double TIER_ZERO_THRESHOLD = 0.3;
constexpr double TIER_ONE_THRESHOLD = 0.65;
constexpr const char* SYSTEM_PROMPT =
    "You are a sentiment classifier for game reviews/comments. Reply ONLY with a JSON object: "
    "{\"polarity\":\"positive\"|\"negative\"|\"neutral\",\"confidence\":0.0..1.0}. No other text.";

double js_sign(double v) {
    if (std::isnan(v)) return v;
    return v > 0 ? 1.0 : v < 0 ? -1.0 : v;
}

double tier_zero_score(std::string_view text, const std::optional<Signal>& signal) {
    const std::string normalized = str::to_lower(text);
    double polarity = 0;
    double hits = 0;
    for (const auto& [word, score] : sentiment_lexicon().polarity) {
        if (normalized.find(str::to_lower(word)) != std::string::npos) {
            polarity += score;
            hits += 1;
        }
    }
    if (signal && signal->votedUp == true) {
        polarity += 1.5;
        hits += 1;
    } else if (signal && signal->votedUp == false) {
        polarity -= 1.5;
        hits += 1;
    }
    // signal?.upvotes && signal.upvotes > 0 (NaN・0 は偽)
    if (signal && signal->upvotes && *signal->upvotes > 0) {
        const double boost = std::min(1.0, js_log10(*signal->upvotes + 1) / 3);
        polarity += boost * (hits > 0 ? js_sign(polarity) : 0);
    }
    return hits > 0 ? std::max(-1.0, std::min(1.0, polarity / std::max(2.0, hits))) : 0;
}

SentimentResult result_from_score(double score, int tier) {
    SentimentResult r;
    r.polarity = score > 0.05 ? "positive" : score < -0.05 ? "negative" : "neutral";
    r.score = score;
    r.confidence = std::fabs(score);
    r.tier = tier;
    return r;
}

std::string build_prompt(std::string_view text) {
    return "Classify the sentiment of this game review/comment:\n\n\"" + str::js_slice(text, 0, 300) + "\"";
}

// Math.max(0, Math.min(1, x)) — NaN は NaN のまま (JS と同じ)。
double js_clamp01(double x) {
    if (std::isnan(x)) return x;
    return std::max(0.0, std::min(1.0, x));
}

std::optional<SentimentResult> invoke(SentimentLlmClient& client, std::string_view text, int tier) {
    SentimentLlmRequest request{SYSTEM_PROMPT, build_prompt(text), 64};
    const SentimentLlmResponse response = client.invoke(request);
    if (!response.ok) return std::nullopt;
    const auto parsed = parse_sentiment_response(response.text);
    if (!parsed) return std::nullopt;
    const double score = parsed->polarity == "positive"   ? parsed->confidence
                         : parsed->polarity == "negative" ? -parsed->confidence
                                                          : 0;
    return SentimentResult{parsed->polarity, score, parsed->confidence, tier};
}

} // namespace

std::optional<ParsedSentiment> parse_sentiment_response(std::string_view raw) {
    // raw.match(/\{[\s\S]*\}/): 最初の "{" から最後の "}" まで (貪欲)。
    const auto open = raw.find('{');
    if (open == std::string_view::npos) return std::nullopt;
    const auto close = raw.rfind('}');
    if (close == std::string_view::npos || close < open) return std::nullopt;
    const auto value = parse_json(raw.substr(open, close - open + 1), {kMaxSentimentResponseBytes, 10'000, 32});
    if (!value) return std::nullopt;
    // String(value.polarity) — object 以外 (配列・数値) では polarity は undefined
    if (!value->is_object()) return std::nullopt;
    const auto it = value->find("polarity");
    if (it == value->end() || !it->is_string()) return std::nullopt;
    const std::string polarity = it->get<std::string>();
    if (polarity != "positive" && polarity != "negative" && polarity != "neutral") return std::nullopt;
    // Number(value.confidence ?? 0)
    const auto c = value->find("confidence");
    const double confidence = (c == value->end() || c->is_null()) ? 0.0 : js_to_number(*c);
    return ParsedSentiment{polarity, js_clamp01(confidence)};
}

SentimentResult cascade_sentiment(std::string_view text, const std::optional<Signal>& signal, const CascadeClients& clients) {
    if (str::trim(text).empty()) return SentimentResult{"neutral", 0, 0, 0};
    const double tier_zero = tier_zero_score(text, signal);
    if (std::fabs(tier_zero) >= TIER_ZERO_THRESHOLD) return result_from_score(tier_zero, 0);
    if (clients.local) {
        const auto local = invoke(*clients.local, text, 1);
        if (local && local->confidence >= TIER_ONE_THRESHOLD) return *local;
    }
    if (clients.main) {
        const auto main = invoke(*clients.main, text, 2);
        if (main) return *main;
        return SentimentResult{"neutral", 0, 0, 2};
    }
    return result_from_score(tier_zero, 0);
}

Json to_json(const SentimentResult& result) {
    Json out = Json::object();
    out["polarity"] = result.polarity;
    out["score"] = result.score;
    out["confidence"] = result.confidence;
    out["tier"] = result.tier;
    return out;
}

} // namespace di::lib::sentiment_core
