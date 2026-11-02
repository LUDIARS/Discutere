#include "flow/db/connection.hpp"

#include <mutex>

#include "base/env.hpp"
#include "base/fsutil.hpp"
#include "flow/db/migrations.hpp"

namespace di::flow::db {
namespace {
std::mutex g_mutex;
sql::DatabasePtr g_instance;
} // namespace

sql::DatabasePtr get_flow_db() {
    std::lock_guard lock(g_mutex);
    if (g_instance) return g_instance;
    // process.env.DATABASE_PATH ?? "./data/discutere.db" (空文字はそのまま使う)
    const std::string db_path = env::get("DATABASE_PATH").value_or("./data/discutere.db");
    const std::string dir = fs::dirname(fs::resolve(db_path));
    if (!dir.empty() && !fs::exists(dir)) fs::ensure_dir(dir);
    auto db = sql::Database::open(db_path);
    db->pragma("journal_mode = WAL");
    db->pragma("foreign_keys = ON");
    apply_flow_migrations(*db);
    g_instance = db;
    return db;
}

void reset_flow_db_for_test() {
    std::lock_guard lock(g_mutex);
    g_instance.reset();
}

} // namespace di::flow::db
