#include "kg_sync/import.hpp"

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include <lorica/input_budget.hpp>

#include "base/json.hpp"
#include "base/strings.hpp"
#include "core/db/js_bind.hpp"
#include "kg_sync/shared_tables.hpp"

namespace di::kg_sync {
namespace {

/** Number(v) || 0 (JSON のスカラーに対して)。 */
double js_number_or_zero(const Json& v) {
    double n = 0;
    if (v.is_number()) n = v.get<double>();
    else if (v.is_boolean()) n = v.get<bool>() ? 1 : 0;
    else if (v.is_string()) n = str::js_number(v.get_ref<const std::string&>()).value_or(0);
    return n == n ? n : 0;  // NaN → 0
}

} // namespace

KgImportResult import_shared_kg(sql::Database& db, std::string_view ndjson, const KgImportLimits& limits) {
    // 全体の大きさと行数を、書き込みを始める前に課金する。
    lorica::input_budget budget({limits.max_bytes, limits.max_lines, limits.max_lines, 1});
    std::vector<std::string_view> lines;
    {
        if (!budget.consume(ndjson.size(), 0, 0)) throw std::length_error("kg-sync import: NDJSON が大きすぎる");
        std::size_t pos = 0;
        while (true) {
            const std::size_t nl = ndjson.find('\n', pos);
            std::string_view line = ndjson.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
            // split(/\r?\n/): 行末の \r は区切りの一部
            if (nl != std::string_view::npos && !line.empty() && line.back() == '\r') line.remove_suffix(1);
            if (!budget.consume(0, 1, 1)) throw std::length_error("kg-sync import: 行が多すぎる");
            lines.push_back(line);
            if (nl == std::string_view::npos) break;
            pos = nl + 1;
        }
    }

    KgImportResult result;
    const std::string wm(WATERMARK_COLUMN);
    std::map<std::string, std::vector<std::string>, std::less<>> column_cache;
    db.transaction([&] {
        for (const auto line : lines) {
            const std::string trimmed = str::trim(line);
            if (trimmed.empty()) continue;
            auto parsed = parse_json(trimmed, {limits.max_line_bytes, limits.max_line_elements, limits.max_line_depth});
            if (!parsed) {
                ++result.ignored;
                continue;
            }
            // TS は parsed.type を読むので、null の行は TypeError で取り込み全体が失敗する。
            if (parsed->is_null()) throw std::runtime_error("kg-sync import: null の行がある");
            const Json& type = core::db::js_field(*parsed, "type");
            if (type.is_string() && type.get_ref<const std::string&>() == "manifest") continue;
            const Json& table_j = core::db::js_field(*parsed, "table");
            const Json& data = core::db::js_field(*parsed, "data");
            if (!table_j.is_string() || !(data.is_object() || data.is_array()) ||
                !is_shared_table(table_j.get_ref<const std::string&>())) {
                ++result.ignored;
                continue;
            }
            const std::string table = table_j.get<std::string>();
            const Json& id = core::db::js_field(data, "id");
            if (!id.is_string() || id.get_ref<const std::string&>().empty()) {
                ++result.ignored;
                continue;
            }
            auto cached = column_cache.find(table);
            if (cached == column_cache.end()) cached = column_cache.emplace(table, table_columns(db, table)).first;
            // master/follower 双方に存在する列だけを書く。id は必須。
            std::vector<std::string> cols;
            for (const auto& c : cached->second)
                if (data.contains(c)) cols.push_back(c);
            bool has_id = false;
            for (const auto& c : cols) has_id = has_id || c == "id";
            if (!has_id) {
                ++result.ignored;
                continue;
            }
            std::string placeholders, col_list, updates;
            sql::Params params;
            for (std::size_t i = 0; i < cols.size(); ++i) {
                const auto& c = cols[i];
                if (i) {
                    placeholders += ", ";
                    col_list += ", ";
                }
                placeholders += "@" + c;
                col_list += "\"" + c + "\"";
                if (c != "id") {
                    if (!updates.empty()) updates += ", ";
                    updates += "\"" + c + "\" = excluded.\"" + c + "\"";
                }
                // better-sqlite3 は boolean/object を直接 bind できないので boolean だけ 1/0 に正規化。
                const Json& v = data[c];
                params.set(c, v.is_boolean() ? sql::Value(v.get<bool>() ? 1.0 : 0.0) : core::db::js_bind(v));
            }
            // LWW: 受信行の updated_at が既存以上のときだけ上書き (古い更新で巻き戻さない)。
            const std::string statement = "INSERT INTO " + table + " (" + col_list + ") VALUES (" + placeholders +
                                          ")\n        ON CONFLICT(id) DO UPDATE SET " + updates +
                                          "\n        WHERE excluded.\"" + wm + "\" >= " + table + ".\"" + wm + "\"";
            const auto info = db.run(statement, params);
            if (info.changes > 0) {
                ++result.applied;
                const double u = js_number_or_zero(core::db::js_field(data, wm.c_str()));
                if (u > result.watermark) result.watermark = u;
            } else {
                ++result.skipped;
            }
        }
    });
    return result;
}

} // namespace di::kg_sync
