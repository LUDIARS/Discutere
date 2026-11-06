#pragma once
// cost_relay_tests.cpp を Praesidium の静的解析 (1 単位 60 秒) に収まるよう分けた各パートが共有する準備。
// src/flow/cost-report.ts / cost-relay.ts の移植の確認 (tests/flow/cost-relay.test.ts の観点 + golden)。
// llm_call_log は w1-C が作る flow の DB スキーマと同じ列をテスト内で合成する。
#include "check.hpp"

#include <atomic>
#include <chrono>
#include <thread>

#include "base/fsutil.hpp"
#include "base/json.hpp"
#include "flow/cost_relay.hpp"
#include "flow/cost_report.hpp"
#include "infra/http_server.hpp"

using namespace di;
using namespace di::flow;

namespace cost_relay_support {

Json fixture() {
    auto text = fs::read_file(test::fixtures_dir() + "/flow/w1e_cost.json");
    if (!text) throw test::Failure("fixture missing");
    return parse_json_or_throw(*text, {16u << 20, 1'000'000, 64});
}

sql::DatabasePtr seeded(const Json& fx) {
    auto db = sql::Database::open(":memory:");
    db->exec(fx["schema"].get<std::string>());
    for (const auto& r : fx["rows"]) {
        sql::Params p;
        for (const auto& v : r) {
            if (v.is_null()) p.add(nullptr);
            else if (v.is_string()) p.add(v.get<std::string>());
            else if (v.is_number_integer()) p.add(v.get<std::int64_t>());
            else p.add(v.get<double>());
        }
        db->run("INSERT INTO llm_call_log (flow, session_id, model, backend, input_tokens, output_tokens, "
                "cache_read_input_tokens, cache_creation_input_tokens, cost_usd, created_at, location) VALUES (?,?,?,?,?,?,?,?,?,?,'loc')",
                p);
    }
    return db;
}

CostReportFilter filter_of(const Json& f) {
    CostReportFilter out;
    if (f.contains("sessionId")) out.sessionId = f["sessionId"].get<std::string>();
    if (f.contains("since")) out.since = f["since"].get<double>();
    if (f.contains("activeSince")) out.activeSince = f["activeSince"].get<double>();
    return out;
}

http::ClientResponse ok_response(std::string body) {
    http::ClientResponse r;
    r.transport_ok = true;
    r.status = 200;
    r.body = std::move(body);
    return r;
}

std::vector<CostFeedRow> sample_rows() {
    CostFeedRow r;
    r.sessionId = "S1";
    r.model = "opus";
    r.backend = "claude-cli";
    r.calls = 2;
    r.inputTokens = 20;
    r.outputTokens = 8;
    r.cacheReadTokens = 200;
    r.cacheCreationTokens = 40;
    r.costUsd = 0.03;
    return {r};
}

} // namespace cost_relay_support
