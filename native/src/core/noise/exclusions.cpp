#include "core/noise/exclusions.hpp"

#include "base/time.hpp"

namespace di::core::noise {

void ensure_exclusion_table(sql::Database& raw) {
    raw.exec(R"(CREATE TABLE IF NOT EXISTS utterance_exclusions (
       utterance_id TEXT PRIMARY KEY,
       reason TEXT,
       created_at INTEGER NOT NULL
     ))");
}

void exclude_utterance(sql::Database& raw, const std::string& utterance_id, const std::optional<std::string>& reason) {
    ensure_exclusion_table(raw);
    // Date.now() は better-sqlite3 で REAL として bind される (INTEGER 列なので整数で保存される)
    raw.run("INSERT OR REPLACE INTO utterance_exclusions (utterance_id, reason, created_at) VALUES (?, ?, ?)",
            {utterance_id, sql::Value(reason), static_cast<double>(time::now_ms())});
}

void restore_utterance(sql::Database& raw, const std::string& utterance_id) {
    ensure_exclusion_table(raw);
    raw.run("DELETE FROM utterance_exclusions WHERE utterance_id = ?", {utterance_id});
}

bool is_excluded(sql::Database& raw, const std::string& utterance_id) {
    ensure_exclusion_table(raw);
    return raw.get("SELECT 1 AS x FROM utterance_exclusions WHERE utterance_id = ?", {utterance_id}).has_value();
}

std::unordered_set<std::string> list_excluded_ids(sql::Database& raw) {
    ensure_exclusion_table(raw);
    std::unordered_set<std::string> out;
    for (const auto& row : raw.all("SELECT utterance_id FROM utterance_exclusions"))
        out.insert(row.str("utterance_id"));
    return out;
}

} // namespace di::core::noise
