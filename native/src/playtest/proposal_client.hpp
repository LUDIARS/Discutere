#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "base/json.hpp"
#include "infra/http_client.hpp"

namespace di::playtest {

using ProposalFetch = std::function<http::ClientResponse(const http::ClientRequest&)>;

Json proposal_payload(const std::string& conclusion_id, const std::string& project_code, const std::string& target_key,
                      const std::string& summary, std::int64_t observed_at);
std::optional<std::string> send_proposal(const std::string& base_url, const Json& payload, const ProposalFetch& fetch);

} // namespace di::playtest
