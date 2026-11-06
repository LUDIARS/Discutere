#include "flow/cost_report.hpp"

#include "base/strings.hpp"
#include "lib/sentiment_core/js_number.hpp"

namespace di::flow {

namespace {

constexpr const char* SELECT_SUMS = R"(
  COUNT(*) AS calls,
  COALESCE(SUM(input_tokens), 0) AS input_tokens,
  COALESCE(SUM(output_tokens), 0) AS output_tokens,
  COALESCE(SUM(cache_read_input_tokens), 0) AS cache_read_input_tokens,
  COALESCE(SUM(cache_creation_input_tokens), 0) AS cache_creation_input_tokens,
  COALESCE(SUM(cost_usd), 0) AS cost_usd)";

// 集計結果は行数が限られる (キーごと 1 行) が、DB の値として上限つきで読む。
const sql::QueryLimits kLimits{1'000'000, 256u << 20};

struct Where {
    std::string sql;
    std::vector<sql::Value> params;
};

Where where_clause(const CostReportFilter& filter) {
    std::vector<std::string> conds;
    Where w;
    if (filter.sessionId && !filter.sessionId->empty()) {
        conds.emplace_back("session_id = ?");
        w.params.emplace_back(*filter.sessionId);
    }
    if (filter.since) {
        conds.emplace_back("created_at >= ?");
        w.params.emplace_back(*filter.since);
    }
    if (filter.activeSince) {
        conds.emplace_back("session_id IN (SELECT session_id FROM llm_call_log WHERE created_at >= ?)");
        w.params.emplace_back(*filter.activeSince);
    }
    w.sql = conds.empty() ? "" : "WHERE " + str::join(conds, " AND ");
    return w;
}

CostRow to_row(const sql::Row& r, const std::string& fallback_key) {
    CostRow row;
    row.key = r.is_null("key") ? fallback_key : r.str("key");
    row.calls = r.num("calls");
    row.inputTokens = r.num("input_tokens");
    row.outputTokens = r.num("output_tokens");
    row.cacheReadTokens = r.num("cache_read_input_tokens");
    row.cacheCreationTokens = r.num("cache_creation_input_tokens");
    row.costUsd = r.num("cost_usd");
    return row;
}

std::string pad_end(const std::string& s, std::size_t width) {
    const std::size_t len = str::js_length(s);
    return len >= width ? s : s + std::string(width - len, ' ');
}

std::string pad_start(const std::string& s, std::size_t width) {
    const std::size_t len = str::js_length(s);
    return len >= width ? s : std::string(width - len, ' ') + s;
}

std::string usd(double n) { return "$" + lib::sentiment_core::js_to_fixed(n, 4); }
std::string num(double n) { return js_number_to_string(n); }

} // namespace

CostReport summarize_cost(sql::Database& db, const CostReportFilter& filter) {
    const Where w = where_clause(filter);
    CostReport report;
    const auto total = db.get(std::string("SELECT NULL AS key, ") + SELECT_SUMS + " FROM llm_call_log " + w.sql, sql::Params(w.params));
    report.total = total ? to_row(*total, "(total)") : CostRow{"(total)"};
    auto group_by = [&](const char* col) {
        std::vector<CostRow> out;
        const std::string q = std::string("SELECT ") + col + " AS key, " + SELECT_SUMS + " FROM llm_call_log " + w.sql +
                              "\n           GROUP BY " + col + " ORDER BY cost_usd DESC, calls DESC";
        for (const auto& r : db.all(q, sql::Params(w.params), kLimits)) out.push_back(to_row(r, "(none)"));
        return out;
    };
    report.bySession = group_by("session_id");
    report.byFlow = group_by("flow");
    report.byBackend = group_by("backend");
    return report;
}

std::vector<CostFeedRow> cost_feed_rows(sql::Database& db, const CostReportFilter& filter) {
    const Where w = where_clause(filter);
    const std::string q = std::string("SELECT session_id, model, backend, ") + SELECT_SUMS +
                          "\n       FROM llm_call_log " + w.sql +
                          "\n       GROUP BY session_id, model, backend\n       ORDER BY session_id";
    std::vector<CostFeedRow> out;
    for (const auto& r : db.all(q, sql::Params(w.params), kLimits)) {
        CostFeedRow row;
        row.sessionId = r.str("session_id");
        row.model = r.opt_str("model");
        row.backend = r.opt_str("backend");
        row.calls = r.num("calls");
        row.inputTokens = r.num("input_tokens");
        row.outputTokens = r.num("output_tokens");
        row.cacheReadTokens = r.num("cache_read_input_tokens");
        row.cacheCreationTokens = r.num("cache_creation_input_tokens");
        row.costUsd = r.num("cost_usd");
        out.push_back(std::move(row));
    }
    return out;
}

std::string format_cost_report(const CostReport& report) {
    auto line = [](const CostRow& r) {
        return "  " + pad_end(r.key, 28) + " calls=" + pad_start(num(r.calls), 5) + " in=" + pad_start(num(r.inputTokens), 8) +
               " out=" + pad_start(num(r.outputTokens), 7) + " cacheR=" + pad_start(num(r.cacheReadTokens), 9) +
               " cacheW=" + pad_start(num(r.cacheCreationTokens), 8) + " cost=" + usd(r.costUsd);
    };
    auto section = [&](const std::string& title, const std::vector<CostRow>& rows) {
        std::vector<std::string> lines{title + ":"};
        for (const auto& r : rows) lines.push_back(line(r));
        return str::join(lines, "\n");
    };
    const CostRow& t = report.total;
    return str::join(
        {
            "=== Discutere LLM cost report ===",
            "TOTAL  calls=" + num(t.calls) + " in=" + num(t.inputTokens) + " out=" + num(t.outputTokens) +
                " cacheRead=" + num(t.cacheReadTokens) + " cacheCreate=" + num(t.cacheCreationTokens) + " cost=" + usd(t.costUsd),
            "(cost_usd: claude-cli=等価API換算の推定 / anthropic従量=未取得でNULL=0)",
            "",
            section("by backend", report.byBackend),
            "",
            section("by flow", report.byFlow),
            "",
            section("by session", report.bySession),
        },
        "\n");
}

Json to_json(const CostRow& r) {
    Json o = Json::object();
    o["key"] = r.key;
    o["calls"] = r.calls;
    o["inputTokens"] = r.inputTokens;
    o["outputTokens"] = r.outputTokens;
    o["cacheReadTokens"] = r.cacheReadTokens;
    o["cacheCreationTokens"] = r.cacheCreationTokens;
    o["costUsd"] = r.costUsd;
    return o;
}

Json to_json(const CostFeedRow& r) {
    Json o = Json::object();
    o["sessionId"] = r.sessionId;
    if (r.model) o["model"] = *r.model;
    if (r.backend) o["backend"] = *r.backend;
    o["calls"] = r.calls;
    o["inputTokens"] = r.inputTokens;
    o["outputTokens"] = r.outputTokens;
    o["cacheReadTokens"] = r.cacheReadTokens;
    o["cacheCreationTokens"] = r.cacheCreationTokens;
    o["costUsd"] = r.costUsd;
    return o;
}

Json to_json(const CostReport& r) {
    auto arr = [](const std::vector<CostRow>& rows) {
        Json a = Json::array();
        for (const auto& x : rows) a.push_back(to_json(x));
        return a;
    };
    Json o = Json::object();
    o["total"] = to_json(r.total);
    o["bySession"] = arr(r.bySession);
    o["byFlow"] = arr(r.byFlow);
    o["byBackend"] = arr(r.byBackend);
    return o;
}

} // namespace di::flow
