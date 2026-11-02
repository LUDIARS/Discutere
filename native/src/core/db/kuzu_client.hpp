#pragma once
// core の索引 DB (src/core/db/kuzu-client.ts)。名前に反して中身は SQLite (better-sqlite3 と同じ)。
// 開くときの pragma (journal_mode=WAL / foreign_keys=ON) と migrations も TS と同じ。
//
// 移していないもの: src/core/db/kuzu-graph-client.ts と src/core/events/kuzu-projection.ts
// (npm kuzu の Kuzu グラフ)。常駐サーバは Kuzu グラフを開かない (createCore に graph を渡す経路が無く、
// 使うのは scripts/rebuild-kuzu.ts 等の CLI だけ) ので、C++ の core は graph 無しの挙動だけを持つ。
#include <optional>
#include <string>

#include "base/sqlite.hpp"

namespace di::core::db {

/** 既定のパス (env DISCATIER_KUZU_PATH、無ければ ./data/discatier.kuzu)。 */
std::string default_kuzu_path();

class KuzuClient {
public:
    /** path を開く (親ディレクトリは作る)。省略時は default_kuzu_path()。失敗は sql::Error。 */
    explicit KuzuClient(const std::optional<std::string>& db_path = std::nullopt);
    KuzuClient(const KuzuClient&) = delete;
    KuzuClient& operator=(const KuzuClient&) = delete;

    sql::Database& raw() { return *db_; }
    const sql::DatabasePtr& raw_ptr() const { return db_; }
    void close();

private:
    sql::DatabasePtr db_;
};

} // namespace di::core::db
