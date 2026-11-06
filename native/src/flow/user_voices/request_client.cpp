#include "flow/user_voices/request_client.hpp"

#include <memory>
#include <algorithm>
#include <chrono>
#include <mutex>

#include "base/json.hpp"
#include "base/url.hpp"
#include "config.hpp"

namespace di::flow::user_voices {
namespace {

http::ClientResponse do_fetch(const VoiceFetch& f, const http::ClientRequest& req) { return f ? f(req) : http::fetch(req); }

http::ClientRequest base_request(const std::string& url, std::chrono::milliseconds timeout) {
    http::ClientRequest req;
    req.url = url;
    req.max_redirects = 0;  // redirect: "manual"
    req.timeout = timeout;
    req.connect_timeout = timeout;
    req.max_response_bytes = kVoiceRequestMaxResponseBytes;
    // 運用者が設定した Voluptas (loopback または LAN の https) へ繋ぐ。URL 検査は先に済ませる。
    req.allow_private_network = true;
    return req;
}

VoiceRequestReceipt request(const std::function<std::string()>& base_url, const cernere_service_token::ServiceTokenProvider& token,
                            const VoiceFetch& fetch_impl, const std::string& request_id, const std::optional<std::string>& theme) {
    const std::string raw = base_url ? base_url() : std::string();
    const auto secure = cernere_service_token::is_secure_service_url(raw);
    auto base = url::parse(raw);
    if (!secure || !*secure || !base) throw VoiceRequestFailure("configuration");
    const std::string origin = base->origin();
    // 認証基盤のエラーに埋もれないよう、先に Vo の応答を確認する。
    const auto health = do_fetch(fetch_impl, base_request(origin + "/health", std::chrono::milliseconds(5000)));
    if (!health.transport_ok) throw VoiceRequestFailure("down");
    if (!health.ok()) throw VoiceRequestFailure(health.status >= 500 ? "down" : "protocol");
    const auto issued = token ? token() : cernere_service_token::ServiceTokenResult{};
    if (!issued.ok) throw VoiceRequestFailure("auth");
    const bool submitting = theme.has_value();
    auto req = base_request(origin + "/api/personas/impression-requests" + (submitting ? "" : "/" + url::encode_component(request_id)),
                            std::chrono::milliseconds(10'000));
    req.method = submitting ? "POST" : "GET";
    req.headers.set("authorization", "Bearer " + issued.token);
    req.headers.set("content-type", "application/json");
    if (submitting) req.body = json_stringify(Json{{"requestId", request_id}, {"theme", *theme}});
    const auto response = do_fetch(fetch_impl, req);
    if (!response.transport_ok) throw VoiceRequestFailure("down", submitting);
    if (!submitting && response.status == 404) return {false, request_id};
    if (!response.ok()) {
        const std::string kind = response.status == 401 || response.status == 403 ? "auth" : response.status >= 500 ? "down" : "protocol";
        throw VoiceRequestFailure(kind, submitting && response.status >= 500);
    }
    auto body = parse_json(response.body, JsonLimits{kVoiceRequestMaxResponseBytes, 10'000, 16});
    if (!body) throw VoiceRequestFailure("protocol", submitting);
    const Json& receipt = json_object(json_object(*body, "data"), "request");
    const auto id = json_opt_string(receipt, "requestId");
    const std::string status = json_string(receipt, "status");
    if (!id || *id != request_id || (status != "accepted" && status != "completed" && status != "cancelled"))
        throw VoiceRequestFailure("protocol", submitting);
    return {status == "accepted" || status == "completed", request_id};
}

} // namespace

VoiceRequestPort create_voice_request_client(std::function<std::string()> base_url, cernere_service_token::ServiceTokenProvider token,
                                             VoiceFetch fetch_impl) {
    // 取り込む値は共有ポインタで持つ (std::function を値で複数取り込むと、静的解析が MSVC の
    // std::function の実装を読み違えて「一時オブジェクトの番地を返す」と誤検知するため)。
    struct Captured {
        std::function<std::string()> base_url;
        cernere_service_token::ServiceTokenProvider token;
        VoiceFetch fetch_impl;
    };
    auto c = std::make_shared<const Captured>(Captured{std::move(base_url), std::move(token), std::move(fetch_impl)});
    VoiceRequestPort port;
    port.submit = [c](const std::string& request_id, const std::string& theme) {
        return request(c->base_url, c->token, c->fetch_impl, request_id, theme);
    };
    port.status = [c](const std::string& request_id) { return request(c->base_url, c->token, c->fetch_impl, request_id, std::nullopt); };
    return port;
}

VoiceRequestPort configured_voice_request() {
    static std::once_flag once;
    static cernere_service_token::ServiceTokenProvider provider;
    std::call_once(once, [] {
        cernere_service_token::ServiceTokenProviderOptions o;
        o.targetProjectKey = "voluptas";
        o.config = [] { return cernere_service_token::service_token_client_config_from_env(); };
        provider = cernere_service_token::create_service_token_provider(std::move(o));
    });
    return create_voice_request_client([] { return config::get_config().flow.userVoices.voluptasBaseUrl; }, provider);
}

} // namespace di::flow::user_voices
