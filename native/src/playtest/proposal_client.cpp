// @implements SPEC-DI-PLAYTEST-PROPOSALS
#include "playtest/proposal_client.hpp"

#include "base/url.hpp"
#include "playtest/contracts/runtime.hpp" /* augur-inject:import:01be4c4b */
#include "contracts/proposal.hpp" /* augur-inject:contract-predicate:ece6acc3 */

namespace di::playtest {

Json proposal_payload(const std::string& conclusion_id, const std::string& project_code, const std::string& target_key,
                      const std::string& summary, std::int64_t observed_at) { return ::augur::invoke_observed(::augur::ContractSpec{"C-7", "d0c90753", "src/playtest/proposal_client.cpp:8"}, di::playtest::checks::proposal, [&]() -> Json { /* augur-inject:contract-wrap:d0c90753 */
    return Json{{"event_key", "di:proposal:" + conclusion_id}, {"source", "di"}, {"kind", "improvement_proposal"},
                {"project_code", project_code}, {"target_key", target_key}, {"origin", "ai"}, {"summary", summary},
                {"observed_at", observed_at}};
}, conclusion_id, project_code, target_key, summary, observed_at); /* augur-inject:contract-end:d0c90753 */ }

std::optional<std::string> send_proposal(const std::string& base_url, const Json& payload, const ProposalFetch& fetch) {
    if (base_url.empty()) return "management_url_not_configured";
    const auto target = url::parse(base_url);
    if (!target || (target->scheme != "http" && target->scheme != "https") ||
        (target->host != "127.0.0.1" && target->host != "::1") || target->target != "/" || !target->fragment.empty())
        return "invalid_management_url";
    http::ClientRequest request;
    request.method = "POST";
    request.url = target->origin() + "/v1/management/events";
    request.headers.set("content-type", "application/json");
    request.body = json_stringify(payload);
    request.connect_timeout = std::chrono::milliseconds(2'000);
    request.timeout = std::chrono::milliseconds(5'000);
    request.max_response_bytes = 64u << 10;
    request.max_redirects = 0;
    request.allow_private_network = true;
    const auto response = fetch(request);
    if (!response.transport_ok) return response.error_kind == http::FetchErrorKind::timeout ? "timeout" : "transport_error";
    if (response.status != 200 && response.status != 201) return "http_" + std::to_string(response.status);
    const auto receipt = parse_json(response.body, JsonLimits{request.max_response_bytes, 1'000, 8});
    if (!receipt || json_string(json_object(*receipt, "event"), "event_key") != json_string(payload, "event_key"))
        return "invalid_acknowledgment";
    return std::nullopt;
}

} // namespace di::playtest
