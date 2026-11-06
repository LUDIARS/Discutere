#include "crawler/sources/raw_store.hpp"

#include "base/fsutil.hpp"
#include "base/time.hpp"

namespace di::crawler::sources {

std::string default_raw_path() { return fs::resolve("./data/external/.raw.sqlite"); }

Json to_json(const RawDocument& v) {
    return Json{{"utteranceId", v.utteranceId}, {"source", v.source}, {"url", v.url}, {"rawText", v.rawText}, {"savedAt", v.savedAt}};
}

namespace {
class SqliteRawStore final : public RawStore {
public:
    explicit SqliteRawStore(sql::DatabasePtr db) : db_(std::move(db)) {}
    void set(const RawStoreInput& input) override {
        db_->run(
            "INSERT INTO source_raw (utterance_id, source, url, raw_text, saved_at)\n"
            "       VALUES (@utteranceId, @source, @url, @rawText, @savedAt)\n"
            "     ON CONFLICT(utterance_id) DO UPDATE SET\n"
            "       source = excluded.source, url = excluded.url, raw_text = excluded.raw_text, saved_at = excluded.saved_at",
            sql::Params::named({{"utteranceId", input.utteranceId},
                                {"source", input.source},
                                {"url", input.url},
                                {"rawText", input.rawText},
                                {"savedAt", input.at.value_or(time::now_ms())}}));
    }
    std::optional<RawDocument> get(const std::string& utterance_id) override {
        auto row = db_->get("SELECT utterance_id, source, url, raw_text, saved_at FROM source_raw WHERE utterance_id = ?", {utterance_id});
        if (!row) return std::nullopt;
        RawDocument d;
        d.utteranceId = row->str("utterance_id");
        d.source = row->str("source");
        d.url = row->str("url");
        d.rawText = row->str("raw_text");
        d.savedAt = row->i64("saved_at");
        return d;
    }
    void close() override { db_->close(); }

private:
    sql::DatabasePtr db_;
};
} // namespace

std::unique_ptr<RawStore> open_raw_store(const std::string& db_path) {
    fs::ensure_dir(fs::dirname(db_path));
    auto db = sql::Database::open(db_path);
    db->pragma("journal_mode = WAL");
    db->exec(
        "CREATE TABLE IF NOT EXISTS source_raw (\n"
        "       utterance_id TEXT PRIMARY KEY,\n"
        "       source TEXT NOT NULL,\n"
        "       url TEXT NOT NULL,\n"
        "       raw_text TEXT NOT NULL,\n"
        "       saved_at INTEGER NOT NULL\n"
        "     )");
    return std::make_unique<SqliteRawStore>(std::move(db));
}

} // namespace di::crawler::sources
