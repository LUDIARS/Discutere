#pragma once
// discutere.db の接続 (src/db/connection.ts)。TS は import 時に開いて drizzle に渡していた。
// C++ は get_main_db() で開く (env DATABASE_PATH、既定 data/discutere.db。WAL + foreign_keys)。
// TS は drizzle-kit push でテーブルを作っていたが、C++ では drizzle-kit が無いので、開いたときに
// 無いテーブルだけを同じ DDL で作る (db::ensure_schema)。既存の DB はそのまま開ける。
#include "base/sqlite.hpp"

namespace di::db {

sql::DatabasePtr get_main_db();
void reset_main_db_for_test();

} // namespace di::db
