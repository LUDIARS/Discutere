#pragma once
// フロー共通 DB マイグレーション (src/flow/db/migrations.ts)。`_flow_migrations` で適用済みを管理し、
// discutere.db に追記する。文と順序は TS と 1 文字も違わない (golden: native/tests/flow_db/migrations_tests.cpp)。
// 既存 DB には未適用のものだけを追加で当てる。ALTER TABLE の "duplicate column name" だけは無視し、
// それ以外の失敗は transaction ごと巻き戻して投げる (失敗した migration を適用済みにしない)。
#include <string>
#include <string_view>
#include <vector>

#include "base/sqlite.hpp"

namespace di::flow::db {

struct FlowMigration {
    std::string id;
    std::vector<std::string> sql;
};

const std::vector<FlowMigration>& flow_migrations();
bool is_ignorable_migration_error(std::string_view stmt, std::string_view message);
void apply_flow_migrations(sql::Database& db);

} // namespace di::flow::db
