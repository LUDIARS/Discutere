#pragma once
// src/flow/voluptas-persona-client.ts の移植 (@implements SPEC-PERSONA-BRIDGE-PERSONA-PULL)。
// Voluptas の persona export を NDJSON のページで取り込む。URL は HTTPS か loopback の HTTP だけ。
// Cernere service token を優先し、発行に失敗した時だけ固定トークンへ落とす (理由コードだけをログし、token は出さない)。
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "base/json.hpp"
#include "cernere_service_token/service_token_client.hpp"
#include "infra/http_client.hpp"

namespace di::flow {

/** Voluptas persona export を呼ぶ service token の宛先 (Cernere の managed project key)。 */
inline constexpr const char* VOLUPTAS_TARGET_PROJECT_KEY = "voluptas";
inline constexpr int kVoluptasMaxPages = 100;
inline constexpr std::size_t kVoluptasMaxPageBytes = 16u << 20;

struct ResolveVoluptasBearerArgs {
    std::optional<cernere_service_token::ServiceTokenProvider> serviceToken;
    std::string fallbackToken;
    std::function<void(const std::string&)> log;
};

/** 使う Bearer を決める。どちらも無ければ std::runtime_error。 */
std::string resolve_voluptas_bearer(const ResolveVoluptasBearerArgs& args);

struct PersonaPullResult {
    std::vector<Json> personas;
    int invalidJsonLines = 0;
    int pages = 0;
};

struct PullVoluptasArgs {
    std::string url;
    /** 従来の固定トークン (service token 発行失敗時のフォールバック)。 */
    std::string token;
    std::optional<cernere_service_token::ServiceTokenProvider> serviceToken;
    std::function<http::ClientResponse(const http::ClientRequest&)> fetchImpl;
    std::function<void(const std::string&)> log;
};

/** 全ページを取り込む。URL 不正・HTTP 失敗・ページ数超過は std::runtime_error。 */
PersonaPullResult pull_voluptas_personas(const PullVoluptasArgs& args);

} // namespace di::flow
