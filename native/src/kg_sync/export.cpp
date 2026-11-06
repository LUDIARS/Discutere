#include "kg_sync/export.hpp"

#include <vector>

#include "base/json.hpp"
#include "base/strings.hpp"
#include "kg_sync/shared_tables.hpp"

namespace di::kg_sync {
namespace {

/** Number(v) || 0 (better-sqlite3 が返す JS 値に対して)。 */
double js_number_or_zero(const sql::Value& v) {
    if (v.is_number()) return v.as_double();
    if (v.is_text()) {
        auto n = str::js_number(v.as_string());
        return n && *n == *n ? *n : 0;
    }
    return 0;
}

} // namespace

KgExportResult export_shared_kg(sql::Database& db, const KgExportOptions& opts) {
    const double since = opts.since.value_or(0);
    double watermark = since;
    std::vector<std::string> row_lines;
    const std::string wm(WATERMARK_COLUMN);
    for (auto table : SHARED_TABLES) {
        const auto cols = table_columns(db, table);
        std::string select;
        for (std::size_t i = 0; i < cols.size(); ++i) {
            if (i) select += ", ";
            select += quote_identifier(cols[i]);
        }
        std::string where = std::string(SHARED_FLAG_COLUMN) + " = 1 AND " + wm + " > @since";
        sql::Params params;
        params.set("since", since);
        if (opts.workspaceId && !opts.workspaceId->empty()) {
            where += " AND workspace_id = @ws";
            params.set("ws", *opts.workspaceId);
        }
        const auto rows = db.all("SELECT " + select + " FROM " + std::string(table) + " WHERE " + where + " ORDER BY " + wm + " ASC",
                                 params, {opts.max_rows_per_table, opts.max_bytes_per_table});
        for (const auto& row : rows) {
            const double u = js_number_or_zero(row[wm]);
            if (u > watermark) watermark = u;
            row_lines.push_back(json_stringify(Json{{"type", "row"}, {"table", std::string(table)}, {"data", row.to_json()}}));
        }
    }
    Json tables = Json::array();
    for (auto t : SHARED_TABLES) tables.push_back(std::string(t));
    const Json manifest = {{"type", "manifest"},
                           {"schemaVersion", KG_SYNC_SCHEMA_VERSION},
                           {"since", since},
                           {"watermark", watermark},
                           {"count", row_lines.size()},
                           {"tables", tables}};
    KgExportResult out;
    out.ndjson = json_stringify(manifest);
    for (const auto& line : row_lines) {
        out.ndjson += '\n';
        out.ndjson += line;
    }
    out.ndjson += '\n';
    out.watermark = watermark;
    out.count = row_lines.size();
    return out;
}

} // namespace di::kg_sync
