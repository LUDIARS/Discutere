#include "cernere_service_token/service_token_client.hpp"

#include <cmath>
#include <memory>
#include <mutex>

#include "base/env.hpp"
#include "base/json.hpp"
#include "base/strings.hpp"
#include "base/time.hpp"
#include "base/url.hpp"

namespace di::cernere_service_token {

namespace {

constexpr std::int64_t REFRESH_MARGIN_MS = 60 * 1000;

ServiceTokenFailureReason reason_for_status(int status) {
    if (status == 401) return "unauthorized";
    if (status == 403) return "scope_undeclared";
    if (status == 404) return "target_not_found";
    return "issuer_error";
}

bool is_scheme_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
}

} // namespace

std::optional<bool> is_secure_service_url(const std::string& raw) {
    const std::string text = str::trim(raw);
    const auto u = url::parse(text);
    if (!u) {
        // http(s) 以外の scheme (ftp:// 等) は JS の URL では読めて「安全でない」になる
        const auto colon = text.find(':');
        if (colon != std::string::npos && colon > 0) {
            bool scheme = (text[0] >= 'a' && text[0] <= 'z') || (text[0] >= 'A' && text[0] <= 'Z');
            for (std::size_t i = 0; i < colon && scheme; ++i) scheme = is_scheme_char(text[i]);
            const std::string s = str::to_lower_ascii(text.substr(0, colon));
            if (scheme && s != "http" && s != "https" && colon + 1 < text.size()) return false;
        }
        return std::nullopt;
    }
    if (u->scheme == "https") return true;
    if (u->scheme == "http") return u->host == "localhost" || u->host == "127.0.0.1" || u->host == "::1";
    return false;  // ws / wss
}

ServiceTokenProvider create_service_token_provider(ServiceTokenProviderOptions options) {
    struct State {
        std::mutex mutex;
        std::optional<std::string> token;
        std::int64_t refresh_at = 0;
    };
    auto state = std::make_shared<State>();
    if (!options.now) options.now = [] { return time::now_ms(); };
    return [state, options]() -> ServiceTokenResult {
        std::lock_guard lock(state->mutex);
        if (state->token && options.now() < state->refresh_at) return {true, *state->token, ""};
        state->token.reset();
        const ServiceTokenClientConfig cfg = options.config ? options.config() : ServiceTokenClientConfig{};
        if (cfg.clientId.empty() || cfg.clientSecret.empty()) return {false, "", "credentials_missing"};
        if (str::trim(cfg.baseUrl).empty()) return {false, "", "base_url_missing"};
        const auto secure = is_secure_service_url(cfg.baseUrl);
        if (!secure) return {false, "", "base_url_missing"};
        if (!*secure) return {false, "", "base_url_insecure"};
        const auto u = url::parse(str::trim(cfg.baseUrl));
        Json body = Json::object();
        body["client_id"] = cfg.clientId;
        body["client_secret"] = cfg.clientSecret;
        body["target_project_key"] = options.targetProjectKey;
        http::ClientRequest req;
        req.method = "POST";
        req.url = u->origin() + "/api/auth/service-token";
        req.headers.set("content-type", "application/json");
        req.headers.set("accept", "application/json");
        req.body = json_stringify(body);
        req.max_redirects = 0;  // credentials を別の宛先へ転送しない
        req.timeout = std::chrono::milliseconds(15'000);
        req.max_response_bytes = kServiceTokenMaxResponseBytes;
        req.allow_private_network = true;  // 設定済みの Cernere (開発では loopback)
        http::ClientResponse res;
        try {
            res = options.fetchImpl ? options.fetchImpl(req) : http::fetch(req);
        } catch (...) {
            return {false, "", "network"};
        }
        if (!res.transport_ok) return {false, "", "network"};
        if (!res.ok()) return {false, "", reason_for_status(res.status)};
        auto parsed = parse_json(res.body, JsonLimits{kServiceTokenMaxResponseBytes, 1'000, 16});
        if (!parsed) return {false, "", "malformed_response"};
        const Json& j = *parsed;
        const bool object = j.is_object();
        const bool token_ok = object && j.contains("accessToken") && j["accessToken"].is_string() &&
                              !j["accessToken"].get_ref<const std::string&>().empty();
        const bool exp_ok = object && j.contains("expiresIn") && j["expiresIn"].is_number() &&
                            std::isfinite(j["expiresIn"].get<double>()) && j["expiresIn"].get<double>() > 0;
        if (!token_ok || !exp_ok) return {false, "", "malformed_response"};
        const std::string token = j["accessToken"].get<std::string>();
        state->token = token;
        state->refresh_at = options.now() + static_cast<std::int64_t>(j["expiresIn"].get<double>() * 1000) - REFRESH_MARGIN_MS;
        return {true, token, ""};
    };
}

ServiceTokenClientConfig service_token_client_config_from_env(const std::optional<std::map<std::string, std::string>>& env) {
    auto get = [&](const char* key) -> std::string {
        if (env) {
            auto it = env->find(key);
            return it == env->end() ? std::string() : it->second;
        }
        return env::get(key).value_or("");
    };
    return {get("CERNERE_BASE_URL"), get("CERNERE_PROJECT_CLIENT_ID"), get("CERNERE_PROJECT_CLIENT_SECRET")};
}

} // namespace di::cernere_service_token
