#include "core/projection/command_handlers/cross/hotspot.hpp"

#include <algorithm>
#include <cstdio>

#include "base/strings.hpp"

namespace di::core::projection::command_handlers::cross {

namespace {
constexpr sql::QueryLimits kMechanicRowLimits{100'000, 64u << 20};

double count_of(sql::Database& raw, std::string_view sql, const sql::Params& params) {
    const auto row = raw.get(sql, params);
    return row ? row->num("c") : 0;
}

/** Number.prototype.toFixed(2) (score は 0.5 刻みなので 2 進で正確に表せる)。 */
std::string to_fixed2(double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.2f", v);
    return buf;
}
} // namespace

std::vector<cache::Rank> run_hotspot_aggregation_job(ReturnTypeCreateCore& core, const std::string& workspace_id) {
    auto& raw = core.raw();
    const auto mechanics = raw.all("SELECT id,name FROM mechanics WHERE workspace_id = ?", {workspace_id}, kMechanicRowLimits);
    std::vector<cache::Rank> ranks;
    for (const auto& m : mechanics) {
        const std::string like = "%" + m.str("name") + "%";
        cache::Rank r;
        r.mechanicId = m.str("id");
        r.name = m.str("name");
        r.utteranceCount = count_of(raw, "SELECT COUNT(*) c FROM utterances WHERE workspace_id = ? AND raw_content LIKE ?", {workspace_id, like});
        r.gapCount = count_of(raw, "SELECT COUNT(*) c FROM design_gaps WHERE workspace_id = ? AND gap_in = ?", {workspace_id, r.mechanicId});
        r.staleCount = count_of(raw, "SELECT COUNT(*) c FROM hypotheses WHERE workspace_id = ? AND stale_flagged = 1 AND statement LIKE ?",
                                {workspace_id, like});
        r.score = r.utteranceCount * 0.5 + r.gapCount * 2 + r.staleCount * 1.5;
        ranks.push_back(std::move(r));
    }
    std::stable_sort(ranks.begin(), ranks.end(), [](const cache::Rank& a, const cache::Rank& b) { return b.score < a.score; });
    cache::set_hotspot_rank_cache(workspace_id, ranks);
    return ranks;
}

std::string handle_hotspot(ReturnTypeCreateCore& core, const std::string& workspace_id) {
    auto cached = cache::get_hotspot_rank_cache(workspace_id);
    const auto ranks = cached ? *cached : run_hotspot_aggregation_job(core, workspace_id);
    std::vector<std::string> lines;
    for (std::size_t i = 0; i < ranks.size() && i < 10; ++i) {
        const auto& r = ranks[i];
        lines.push_back(std::to_string(i + 1) + ". " + r.name + " score=" + to_fixed2(r.score) + " (u:" + js_number_to_string(r.utteranceCount) +
                        " g:" + js_number_to_string(r.gapCount) + " s:" + js_number_to_string(r.staleCount) + ")");
    }
    const std::string text = str::join(lines, "\n");
    return text.empty() ? std::string("no hotspots") : text;
}

} // namespace di::core::projection::command_handlers::cross
