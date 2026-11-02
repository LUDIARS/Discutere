#include "cernere_service_token/cernere_public_keys.hpp"

#include <stdexcept>

#include "base/crypto.hpp"
#include "base/strings.hpp"
#include "base/time.hpp"
#include "base/url.hpp"

namespace di::cernere_service_token {

std::vector<Ed25519PublicKey> parse_well_known_keys(const Json& body) {
    if (!body.is_object() || !body.contains("keys") || !body["keys"].is_array())
        throw std::runtime_error("cernere public key response has no keys");
    std::vector<Ed25519PublicKey> out;
    for (const auto& entry : body["keys"]) {
        if (!entry.is_object() || !entry.contains("public_key") || !entry["public_key"].is_string()) continue;
        const auto& value = entry["public_key"].get_ref<const std::string&>();
        if (value.empty()) continue;
        const auto raw = crypto::base64_decode(value);
        if (!raw) throw PasetoFormatError("Ed25519 public key must be 32 bytes");
        out.push_back(ed25519_public_key_from_raw(*raw));
    }
    return out;
}

PublicKeyLoader create_cernere_public_key_loader(CernerePublicKeyLoaderOptions options) {
    struct State {
        std::mutex mutex;
        std::optional<std::vector<Ed25519PublicKey>> keys;
        std::int64_t expires_at = 0;
        std::int64_t failed_until = 0;
    };
    auto state = std::make_shared<State>();
    if (!options.now) options.now = [] { return time::now_ms(); };
    return [state, options]() -> std::vector<Ed25519PublicKey> {
        std::lock_guard lock(state->mutex);
        const std::int64_t at = options.now();
        if (state->keys && at < state->expires_at) return *state->keys;
        if (at < state->failed_until) throw std::runtime_error("cernere public key fetch is backing off");
        const std::string base = str::trim(options.baseUrl ? options.baseUrl() : std::string());
        if (base.empty()) throw std::runtime_error("CERNERE_BASE_URL is not set");
        try {
            const auto u = url::parse(base);
            if (!u) throw std::runtime_error("Invalid URL");
            http::ClientRequest req;
            req.url = u->origin() + "/.well-known/cernere-public-key";
            req.headers.set("accept", "application/json");
            req.max_redirects = 0;  // redirect: "manual"
            req.timeout = std::chrono::milliseconds(10'000);
            req.max_response_bytes = kPublicKeyMaxResponseBytes;
            req.allow_private_network = true;  // 設定済みの Cernere (開発では loopback)
            const auto res = options.fetchImpl ? options.fetchImpl(req) : http::fetch(req);
            if (!res.transport_ok) throw std::runtime_error("cernere public key fetch failed: " + res.error);
            if (!res.ok()) throw std::runtime_error("cernere public key fetch failed with status " + std::to_string(res.status));
            std::string error;
            auto body = parse_json(res.body, JsonLimits{kPublicKeyMaxResponseBytes, 10'000, 16}, &error);
            if (!body) throw std::runtime_error("cernere public key response is not JSON: " + error);
            auto keys = parse_well_known_keys(*body);
            if (keys.empty()) throw std::runtime_error("cernere public key response is empty");
            state->keys = keys;
            state->expires_at = at + kPublicKeyCacheTtlMs;
            return keys;
        } catch (...) {
            state->failed_until = at + kPublicKeyFailureBackoffMs;
            throw;
        }
    };
}

} // namespace di::cernere_service_token
