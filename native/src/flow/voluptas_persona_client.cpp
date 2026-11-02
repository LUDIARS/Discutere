#include "flow/voluptas_persona_client.hpp"

#include <stdexcept>

#include "base/log.hpp"
#include "base/url.hpp"
#include "flow/persona_import_document.hpp"

namespace di::flow {

std::string resolve_voluptas_bearer(const ResolveVoluptasBearerArgs& args) {
    const auto log = args.log ? args.log : [](const std::string& line) { log::warn(line); };
    if (args.serviceToken && *args.serviceToken) {
        const auto issued = (*args.serviceToken)();
        if (issued.ok) return issued.token;
        if (!args.fallbackToken.empty()) {
            log("[persona-import] cernere service token unavailable (reason=" + issued.reason + "); using fixed token");
            return args.fallbackToken;
        }
        throw std::runtime_error("Cernere service token unavailable (reason=" + issued.reason +
                                 ") and DISCUTERE_VOLUPTAS_EXPORT_TOKEN is not set");
    }
    if (args.fallbackToken.empty()) throw std::runtime_error("DISCUTERE_VOLUPTAS_EXPORT_TOKEN is required");
    return args.fallbackToken;
}

PersonaPullResult pull_voluptas_personas(const PullVoluptasArgs& args) {
    const auto secure = cernere_service_token::is_secure_service_url(args.url);
    if (!secure) throw std::runtime_error("Invalid URL");
    if (!*secure) throw std::runtime_error("Voluptas export URL must use HTTPS (HTTP is allowed only on loopback)");
    const auto endpoint = url::parse(args.url);
    const std::string bearer = resolve_voluptas_bearer({args.serviceToken, args.token, args.log});

    PersonaPullResult out;
    std::optional<std::string> cursor;
    do {
        if (out.pages >= kVoluptasMaxPages) throw std::runtime_error("Voluptas export exceeded the page limit");
        std::string page_url = args.url;
        if (cursor) {
            // pageUrl.searchParams.set("cursor", cursor)
            auto params = url::parse_query(endpoint->query);
            bool replaced = false;
            std::vector<std::pair<std::string, std::string>> next;
            for (auto& p : params) {
                if (p.first == "cursor") {
                    if (!replaced) next.emplace_back("cursor", *cursor);
                    replaced = true;
                    continue;
                }
                next.push_back(std::move(p));
            }
            if (!replaced) next.emplace_back("cursor", *cursor);
            page_url = endpoint->origin() + endpoint->path + "?" + url::build_query(next);
        }
        http::ClientRequest req;
        req.url = page_url;
        req.headers.set("accept", "application/x-ndjson");
        req.headers.set("authorization", "Bearer " + bearer);
        // Authorization を別 endpoint へ転送しない。redirect は拒否する (redirect: "manual")。
        req.max_redirects = 0;
        req.timeout = std::chrono::milliseconds(60'000);
        req.max_response_bytes = kVoluptasMaxPageBytes;
        req.allow_private_network = true;  // 設定済みの Voluptas (loopback を許す。URL 検査は上で済ませた)
        const auto res = args.fetchImpl ? args.fetchImpl(req) : http::fetch(req);
        if (!res.transport_ok) throw std::runtime_error("Voluptas export request failed: " + res.error);
        if (!res.ok()) throw std::runtime_error("Voluptas export request failed with status " + std::to_string(res.status));
        auto parsed = parse_persona_document(res.body);
        for (auto& p : parsed.personas) out.personas.push_back(std::move(p));
        out.invalidJsonLines += parsed.invalidJsonLines;
        out.pages += 1;
        const auto next = res.headers.get("x-next-cursor");
        cursor = next && !next->empty() ? next : std::nullopt;
    } while (cursor);
    return out;
}

} // namespace di::flow
