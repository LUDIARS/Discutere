#pragma once
// core の SQLite スキーマ (src/core/db/schema.ts)。`_migrations` で適用済みを管理する。
// 文は TS と 1 文字も違わない (sqlite_master に残る文が一致すること、既存 DB をそのまま開けることを
// native/tests/core/schema_tests.cpp が golden で確かめる)。
#include <string>
#include <vector>

#include "base/sqlite.hpp"

namespace di::core::db {

struct Migration {
    std::string id;
    std::vector<std::string> sql;
};

/** 適用順の migration 一覧。 */
const std::vector<Migration>& core_migrations();

/**
 * 未適用の migration を 1 つの transaction で適用する。
 * TS と同じく各文の失敗は無視する (additive migration が既に当たっている場合の no-op)。
 */
void apply_migrations(sql::Database& db);

} // namespace di::core::db
