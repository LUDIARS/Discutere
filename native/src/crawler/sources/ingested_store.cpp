#include "crawler/sources/ingested_store.hpp"

#include "base/fsutil.hpp"
#include "base/time.hpp"

namespace di::crawler::sources {

std::string default_ingested_path() { return fs::resolve("./data/external/.ingested.sqlite"); }

namespace {
class SqliteIngestedStore final : public IngestedStore {
public:
    explicit SqliteIngestedStore(sql::DatabasePtr db) : db_(std::move(db)) {}
    bool has(const ExternalSource& source, const std::string& native_id) override {
        return db_->get("SELECT 1 FROM ingested WHERE source = ? AND native_id = ?", {source, native_id}).has_value();
    }
    void add(const ExternalSource& source, const std::string& native_id, std::optional<std::int64_t> at) override {
        db_->run("INSERT OR IGNORE INTO ingested (source, native_id, imported_at) VALUES (?, ?, ?)",
                 {source, native_id, at.value_or(time::now_ms())});
    }
    void close() override { db_->close(); }

private:
    sql::DatabasePtr db_;
};
} // namespace

std::unique_ptr<IngestedStore> open_ingested_store(const std::string& db_path) {
    fs::ensure_dir(fs::dirname(db_path));
    auto db = sql::Database::open(db_path);
    db->pragma("journal_mode = WAL");
    db->exec(
        "CREATE TABLE IF NOT EXISTS ingested (source TEXT NOT NULL, native_id TEXT NOT NULL, imported_at INTEGER NOT NULL, PRIMARY KEY (source, native_id))");
    return std::make_unique<SqliteIngestedStore>(std::move(db));
}

} // namespace di::crawler::sources
