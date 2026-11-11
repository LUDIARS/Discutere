#include "flow/db/connection.hpp"

#include <mutex>

#include "base/env.hpp"
#include "base/fsutil.hpp"
#include "flow/db/migrations.hpp"

namespace di::flow::db {
namespace {
// g_mutex は g_instance の読み書きだけを守る。開く処理 (pragma・migration で DB 自身の mutex を取る) は
// g_open_mutex の下で行い、g_mutex を持ったまま DB の mutex を取らない。DB の transaction の中から
// この関数を呼ぶ経路 (DB の mutex → g_mutex) と、ロックの順序が逆にならないようにするため。
std::mutex g_mutex;
std::mutex g_open_mutex;
sql::DatabasePtr g_instance;

sql::DatabasePtr current_instance() {
    std::lock_guard lock(g_mutex);
    return g_instance;
}
} // namespace

sql::DatabasePtr get_flow_db() {
    if (auto db = current_instance()) return db;
    std::lock_guard open_lock(g_open_mutex);
    if (auto db = current_instance()) return db;
    // process.env.DATABASE_PATH ?? "./data/discutere.db" (空文字はそのまま使う)
    const std::string db_path = env::get("DATABASE_PATH").value_or("./data/discutere.db");
    const std::string dir = fs::dirname(fs::resolve(db_path));
    if (!dir.empty() && !fs::exists(dir)) fs::ensure_dir(dir);
    auto db = sql::Database::open(db_path);
    db->pragma("journal_mode = WAL");
    db->pragma("foreign_keys = ON");
    apply_flow_migrations(*db);
    {
        std::lock_guard lock(g_mutex);
        g_instance = db;
    }
    return db;
}

void reset_flow_db_for_test() {
    // 閉じる処理 (DB の mutex を取る) は g_mutex の外で行う。
    sql::DatabasePtr old;
    {
        std::lock_guard lock(g_mutex);
        old.swap(g_instance);
    }
    old.reset();
}

} // namespace di::flow::db
