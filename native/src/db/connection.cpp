#include "db/connection.hpp"

#include <mutex>

#include "base/env.hpp"
#include "base/fsutil.hpp"
#include "db/schema.hpp"

namespace di::db {
namespace {
std::mutex g_mutex;
sql::DatabasePtr g_instance;
} // namespace

sql::DatabasePtr get_main_db() {
    std::lock_guard lock(g_mutex);
    if (g_instance) return g_instance;
    // process.env.DATABASE_PATH || "data/discutere.db"
    auto v = env::get("DATABASE_PATH");
    const std::string db_path = v && !v->empty() ? *v : std::string("data/discutere.db");
    const std::string dir = fs::dirname(db_path);
    if (!dir.empty() && !fs::exists(dir)) fs::ensure_dir(dir);
    auto db = sql::Database::open(db_path);
    db->pragma("journal_mode = WAL");
    db->pragma("foreign_keys = ON");
    ensure_schema(*db);
    g_instance = db;
    return db;
}

void reset_main_db_for_test() {
    std::lock_guard lock(g_mutex);
    g_instance.reset();
}

} // namespace di::db
