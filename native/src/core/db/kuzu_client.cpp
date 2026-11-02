#include "core/db/kuzu_client.hpp"

#include "base/env.hpp"
#include "base/fsutil.hpp"
#include "core/db/schema.hpp"

namespace di::core::db {

std::string default_kuzu_path() {
    // process.env.DISCATIER_KUZU_PATH || "./data/discatier.kuzu" (空文字は未設定扱い)
    auto v = env::get("DISCATIER_KUZU_PATH");
    return v && !v->empty() ? *v : std::string("./data/discatier.kuzu");
}

KuzuClient::KuzuClient(const std::optional<std::string>& db_path) {
    // TS の既定引数は undefined のときだけ効く (空文字は path.resolve("") = cwd になる)
    const std::string abs = fs::resolve(db_path ? *db_path : default_kuzu_path());
    fs::ensure_dir(fs::dirname(abs));
    db_ = sql::Database::open(abs);
    db_->pragma("journal_mode = WAL");
    db_->pragma("foreign_keys = ON");
    apply_migrations(*db_);
}

void KuzuClient::close() {
    if (db_) db_->close();
}

} // namespace di::core::db
