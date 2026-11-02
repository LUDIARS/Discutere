#pragma once
// src/flow/cost-report.ts の移植: llm_call_log のコスト/トークン集計 (内部検証用)。
// セッション別・フロー別・backend 別に呼び出し回数 / トークン / 推定コストを合算する。
#include <optional>
#include <string>
#include <vector>

#include "base/json.hpp"
#include "base/sqlite.hpp"

namespace di::flow {

struct CostRow {
    std::string key;
    double calls = 0;
    double inputTokens = 0;
    double outputTokens = 0;
    double cacheReadTokens = 0;
    double cacheCreationTokens = 0;
    double costUsd = 0;
};

struct CostReport {
    CostRow total;
    std::vector<CostRow> bySession;
    std::vector<CostRow> byFlow;
    std::vector<CostRow> byBackend;
};

struct CostReportFilter {
    /** 単一セッションに絞る (空文字は絞らない)。 */
    std::optional<std::string> sessionId;
    /** 行を created_at >= since (epoch ms) で絞る。 */
    std::optional<double> since;
    /** since 以降に活動したセッションの全行を含める (relay 用、累積サマリを保つ)。 */
    std::optional<double> activeSince;
};

CostReport summarize_cost(sql::Database& db, const CostReportFilter& filter = {});

/** cost-feed への PUSH 1 行 = (session × model × backend) 別サマリ。 */
struct CostFeedRow {
    std::string sessionId;
    std::optional<std::string> model;
    std::optional<std::string> backend;
    double calls = 0;
    double inputTokens = 0;
    double outputTokens = 0;
    double cacheReadTokens = 0;
    double cacheCreationTokens = 0;
    double costUsd = 0;
};

std::vector<CostFeedRow> cost_feed_rows(sql::Database& db, const CostReportFilter& filter = {});

/** CostReport を人間可読の文字列にする (CLI 出力用)。 */
std::string format_cost_report(const CostReport& report);

Json to_json(const CostRow& r);
Json to_json(const CostFeedRow& r);
Json to_json(const CostReport& r);

} // namespace di::flow
