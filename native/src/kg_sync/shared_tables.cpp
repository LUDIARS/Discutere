#include "kg_sync/shared_tables.hpp"

#include <stdexcept>

namespace di::kg_sync {

bool is_shared_table(std::string_view name) {
    for (auto t : SHARED_TABLES)
        if (t == name) return true;
    return false;
}

std::string quote_identifier(std::string_view name) {
    std::string out = "\"";
    for (char c : name) {
        if (c == '"') out += '"';
        out += c;
    }
    out += '"';
    return out;
}

std::vector<std::string> table_columns(sql::Database& db, std::string_view table) {
    std::vector<std::string> cols;
    for (const auto& row : db.all("PRAGMA table_info(" + quote_identifier(table) + ")")) cols.push_back(row.str("name"));
    if (cols.empty()) throw std::runtime_error("tableColumns: テーブルが存在しません: " + std::string(table));
    return cols;
}

} // namespace di::kg_sync
