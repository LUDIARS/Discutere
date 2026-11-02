#include "core/vectors/embedder.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "base/json.hpp"
#include "base/log.hpp"
#include "base/strings.hpp"
#include "base/url.hpp"
#include "core/vectors/vector_math.hpp"
#include "infra/http_client.hpp"

namespace di::core::vectors {
namespace {

bool is_scheme_char(char c, bool first) {
    const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    if (first) return alpha;
    return alpha || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
}

/** HTTP の標準の理由句 (fetch の res.statusText の代わり。ログ表示だけに使う)。 */
std::string reason_phrase(int status) {
    switch (status) {
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 413: return "Payload Too Large";
    case 422: return "Unprocessable Entity";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    default: return "";
    }
}

class OpenAiCompatEmbedder final : public EmbeddingClient {
public:
    OpenAiCompatEmbedder(EmbeddingConfig config, std::string url, EmbedderHooks hooks, EmbedderLimits limits)
        : config_(std::move(config)), url_(std::move(url)), hooks_(std::move(hooks)), limits_(limits) {
        if (!hooks_.warn) hooks_.warn = [](const std::string& m) { log::warn("[embedder] " + m); };
        if (!hooks_.sleep) hooks_.sleep = default_embed_retry_sleep;
        retry_ = hooks_.retry.value_or(DEFAULT_EMBED_RETRY);
    }

    std::vector<std::vector<double>> embed(const std::vector<std::string>& texts) override {
        std::vector<std::vector<double>> out;
        if (texts.empty()) return out;
        // 1e9 を超える指定は 1 バッチとみなす (size_t への変換を安全にする)
        const std::size_t batch_size = config_.batchSize >= 1e9 ? 1'000'000'000 : static_cast<std::size_t>(config_.batchSize);
        for (std::size_t i = 0; i < texts.size(); i += batch_size) {
            const std::vector<std::string> batch(texts.begin() + static_cast<std::ptrdiff_t>(i),
                                                 texts.begin() + static_cast<std::ptrdiff_t>(std::min(texts.size(), i + batch_size)));
            auto vectors = with_embed_retry([&] { return embed_batch(batch); }, retry_, hooks_.warn, hooks_.sleep);
            for (auto& v : vectors) out.push_back(std::move(v));
        }
        return out;
    }

private:
    std::vector<std::vector<double>> embed_batch(const std::vector<std::string>& texts) {
        http::ClientRequest req;
        req.method = "POST";
        req.url = url_;
        req.headers.set("content-type", "application/json");
        if (config_.apiKey && !config_.apiKey->empty()) req.headers.set("authorization", "Bearer " + *config_.apiKey);
        Json input = Json::array();
        for (const auto& t : texts) input.push_back(t);
        req.body = json_stringify(Json{{"model", config_.model}, {"input", std::move(input)}});
        const auto timeout = std::chrono::milliseconds(static_cast<long long>(std::min(config_.timeoutMs, 1e12)));
        req.timeout = timeout;
        req.connect_timeout = std::min<std::chrono::milliseconds>(timeout, std::chrono::milliseconds(10'000));
        req.max_response_bytes = limits_.max_response_bytes;
        req.allow_private_network = true;  // 設定済みのローカル/社内の埋め込みサービス
        req.max_redirects = 0;             // fetch の redirect: "error"
        const auto res = http::fetch(req);
        if (!res.transport_ok) throw EmbeddingTransportError("embedding request failed: " + res.error);
        if ((res.status == 301 || res.status == 302 || res.status == 303 || res.status == 307 || res.status == 308) &&
            res.headers.has("location"))
            throw EmbeddingTransportError("embedding request failed: unexpected redirect");
        if (res.status < 200 || res.status >= 300) {
            // 応答本文は入力テキストや上流設定を含み得るためログへ伝播させない。
            throw EmbeddingHttpError(res.status, reason_phrase(res.status));
        }
        std::string parse_error;
        auto json = parse_json(res.body, {limits_.max_response_bytes, limits_.max_json_elements, 16}, &parse_error);
        if (!json) throw std::runtime_error("embedding 応答の JSON が不正: " + parse_error);
        // TS は json.data を読む。null は TypeError (fetch の失敗と同じく一過性に分類される)。
        if (json->is_null()) throw EmbeddingTransportError("embedding 応答が null");
        Json data = Json::array();
        if (json->is_object() && json->contains("data") && !(*json)["data"].is_null()) data = (*json)["data"];
        if (!data.is_array() || data.size() != texts.size()) {
            const std::string got = data.is_array() ? std::to_string(data.size()) : "undefined";
            throw std::runtime_error("embedding 応答件数不一致: 入力 " + std::to_string(texts.size()) + " 件 / 応答 " + got + " 件");
        }
        // index フィールドで入力順に整列 (OpenAI 互換実装は順序保証が曖昧なため明示)。
        std::vector<std::optional<std::vector<double>>> out(texts.size());
        std::optional<std::size_t> dimension;
        for (std::size_t i = 0; i < data.size(); ++i) {
            const Json& d = data[i];
            if (d.is_null()) throw EmbeddingTransportError("embedding 応答の要素が null");
            double idx = static_cast<double>(i);
            if (d.is_object() && d.contains("index") && d["index"].is_number()) idx = d["index"].get<double>();
            if (!std::isfinite(idx) || std::floor(idx) != idx || idx < 0 || idx >= static_cast<double>(texts.size()) ||
                out[static_cast<std::size_t>(idx)])
                throw std::runtime_error("embedding 応答の index が不正または重複している (index=" + js_number_to_string(idx) + ")");
            const auto slot = static_cast<std::size_t>(idx);
            std::optional<std::vector<double>> vec;
            if (d.is_object() && d.contains("embedding")) vec = to_embedding_vector(d["embedding"]);
            if (!vec) throw std::runtime_error("embedding 応答に有効なベクトルが無い (index=" + js_number_to_string(idx) + ")");
            if (dimension && vec->size() != *dimension) throw std::runtime_error("embedding 応答のベクトル次元が一致しない");
            dimension = vec->size();
            out[slot] = std::move(vec);
        }
        std::vector<std::vector<double>> result;
        result.reserve(out.size());
        for (std::size_t idx = 0; idx < out.size(); ++idx) {
            if (!out[idx]) throw std::runtime_error("embedding 応答に index=" + std::to_string(idx) + " が無い");
            result.push_back(std::move(*out[idx]));
        }
        return result;
    }

    EmbeddingConfig config_;
    std::string url_;
    EmbedderHooks hooks_;
    EmbedderLimits limits_;
    EmbedRetryOptions retry_;
};

} // namespace

std::string embedding_endpoint(const EmbeddingConfig& config) {
    std::string base = config.baseUrl;
    while (!base.empty() && base.back() == '/') base.pop_back();
    const std::string s = base + "/embeddings";
    // scheme (new URL が読めない形は「valid URL」の文言)
    const auto colon = s.find(':');
    bool scheme_ok = colon != std::string::npos && colon > 0;
    for (std::size_t i = 0; scheme_ok && i < colon; ++i) scheme_ok = is_scheme_char(s[i], i == 0);
    if (!scheme_ok) throw std::invalid_argument("embedding.baseUrl must be a valid HTTP(S) URL");
    const std::string scheme = str::to_lower_ascii(s.substr(0, colon));
    // 認証情報 (user:pass@) を含む URL は拒否する
    bool credentials = false;
    if (s.compare(colon, 3, "://") == 0) {
        const std::size_t start = colon + 3;
        std::size_t end = s.find_first_of("/?#", start);
        if (end == std::string::npos) end = s.size();
        credentials = s.find('@', start) < end;
    }
    if ((scheme != "http" && scheme != "https") || credentials)
        throw std::invalid_argument("embedding.baseUrl must be an HTTP(S) URL without embedded credentials");
    if (s.find('?') != std::string::npos || s.find('#') != std::string::npos)
        throw std::invalid_argument("embedding.baseUrl must not contain a query string or fragment");
    if (!url::parse(s)) throw std::invalid_argument("embedding.baseUrl must be a valid HTTP(S) URL");
    return s;
}

std::shared_ptr<EmbeddingClient> create_open_ai_compat_embedder(const EmbeddingConfig& config, EmbedderHooks hooks,
                                                                 EmbedderLimits limits) {
    if (!std::isfinite(config.batchSize) || std::floor(config.batchSize) != config.batchSize || config.batchSize <= 0)
        throw std::invalid_argument("embedding.batchSize must be a positive integer");
    if (!std::isfinite(config.timeoutMs) || config.timeoutMs <= 0)
        throw std::invalid_argument("embedding.timeoutMs must be a positive number");
    if (str::trim(config.model).empty()) throw std::invalid_argument("embedding.model must not be empty");
    std::string url = embedding_endpoint(config);
    return std::make_shared<OpenAiCompatEmbedder>(config, std::move(url), std::move(hooks), limits);
}

} // namespace di::core::vectors
