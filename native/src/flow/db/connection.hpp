#pragma once
// フロー DB コネクション (src/flow/db/connection.ts)。
// discutere.db (env DATABASE_PATH、既定 ./data/discutere.db) を開き、WAL / foreign_keys と
// flow migrations を適用済みの状態で返す。プロセスで 1 つを共有する (sql::Database は内部で直列化する)。
#include "base/sqlite.hpp"

namespace di::flow::db {

sql::DatabasePtr get_flow_db();
/** テスト用: インスタンスを捨てて次の get_flow_db で開き直す。 */
void reset_flow_db_for_test();

} // namespace di::flow::db
