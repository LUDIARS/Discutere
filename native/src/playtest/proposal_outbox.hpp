#pragma once

#include <string>

#include "base/sqlite.hpp"
#include "playtest/proposal_client.hpp"

namespace di::playtest {

struct ProposalDestination {
    std::string baseUrl;
    std::string projectCode;
};

void ensure_proposal_outbox(sql::Database& db);
void enqueue_playtest_proposal(sql::Database& db, const std::string& gap_id, const std::string& summary, std::int64_t now);
void deliver_next_proposal(sql::Database& db, const ProposalDestination& destination, std::int64_t now,
                           const ProposalFetch& fetch = http::fetch);

} // namespace di::playtest
