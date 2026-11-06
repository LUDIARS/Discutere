#pragma once
// src/flow/user-voices/request-client.ts の移植: Voluptas の永続受付 API (感想集めの依頼)。
// 読み取り用固定トークンを依頼権限へ転用しない (Cernere service token だけを使う)。
// - URL は HTTPS (loopback のみ HTTP)、userinfo は拒否 (configuration)
// - 先に /health を確かめる (期限 5 秒)。依頼/照会は期限 10 秒。redirect は追わない。応答は 64KiB まで
// - token はログ・エラー文に出さない
#include <functional>
#include <stdexcept>
#include <string>

#include "cernere_service_token/service_token_client.hpp"
#include "flow/user_voices/preparation.hpp"
#include "infra/http_client.hpp"

namespace di::flow::user_voices {

class VoiceRequestFailure : public std::runtime_error {
public:
    /** kind: "down" | "auth" | "configuration" | "protocol" */
    VoiceRequestFailure(std::string kind, bool delivery_unknown = false)
        : std::runtime_error("Voluptas request: " + kind), kind_(std::move(kind)), delivery_unknown_(delivery_unknown) {}
    const std::string& kind() const { return kind_; }
    bool delivery_unknown() const { return delivery_unknown_; }

private:
    std::string kind_;
    bool delivery_unknown_;
};

inline constexpr std::size_t kVoiceRequestMaxResponseBytes = 64 * 1024;

using VoiceFetch = std::function<http::ClientResponse(const http::ClientRequest&)>;

VoiceRequestPort create_voice_request_client(std::function<std::string()> base_url, cernere_service_token::ServiceTokenProvider token,
                                             VoiceFetch fetch_impl = {});

/** config flow.userVoices.voluptasBaseUrl + Cernere service token (targetProjectKey "voluptas")。 */
VoiceRequestPort configured_voice_request();

} // namespace di::flow::user_voices
