#include "crawler/sources/attribution_store.hpp"

#include "base/fsutil.hpp"
#include "base/strings.hpp"
#include "base/time.hpp"
#include "config.hpp"
#include "core/kg_registry.hpp"

namespace di::crawler::sources {

namespace {

Attribution to_attribution(const sql::Row& row) {
    Attribution a;
    a.utteranceId = row.str("utterance_id");
    a.source = row.str("source");
    a.sourceUrl = row.str("source_url");
    a.nativeId = row.str("native_id");
    a.gameSlug = row.opt_str("game_slug");
    a.importedAt = row.num("imported_at");
    return a;
}

class SqliteAttributionStore : public AttributionStore {
public:
    explicit SqliteAttributionStore(sql::DatabasePtr db) : db_(std::move(db)) {}
    ~SqliteAttributionStore() override = default;

    void set(const AttributionInput& input) override {
        db_->run(R"SQL(INSERT INTO source_attribution (utterance_id, source, source_url, native_id, game_slug, imported_at)
       VALUES (@utteranceId, @source, @sourceUrl, @nativeId, @gameSlug, @importedAt)
     ON CONFLICT(utterance_id) DO UPDATE SET
       source = excluded.source, source_url = excluded.source_url,
       native_id = excluded.native_id, game_slug = excluded.game_slug,
       imported_at = excluded.imported_at)SQL",
                 sql::Params::named({{"utteranceId", input.utteranceId},
                                     {"source", input.source},
                                     {"sourceUrl", input.sourceUrl},
                                     {"nativeId", input.nativeId},
                                     {"gameSlug", input.gameSlug},
                                     {"importedAt", input.at ? *input.at : static_cast<double>(time::now_ms())}}));
    }

    std::optional<Attribution> get(const std::string& utterance_id) override {
        auto row = db_->get(
            "SELECT utterance_id, source, source_url, native_id, game_slug, imported_at FROM source_attribution WHERE utterance_id = ?",
            {utterance_id});
        if (!row) return std::nullopt;
        return to_attribution(*row);
    }

    std::vector<Attribution> list_by_source(const std::string& source, const std::optional<std::string>& game_slug, double limit) override {
        const auto rows = game_slug && !game_slug->empty()
            ? db_->all("SELECT * FROM source_attribution WHERE source = ? AND game_slug = ? ORDER BY imported_at DESC LIMIT ?",
                       {source, *game_slug, limit}, kAttributionListLimits)
            : db_->all("SELECT * FROM source_attribution WHERE source = ? ORDER BY imported_at DESC LIMIT ?", {source, limit},
                       kAttributionListLimits);
        std::vector<Attribution> out;
        for (const auto& r : rows) out.push_back(to_attribution(r));
        return out;
    }

    std::vector<SourceCount> counts_by_source() override {
        const auto rows = db_->all(R"SQL(SELECT source, COUNT(*) AS count, MAX(imported_at) AS last_imported_at,
                  GROUP_CONCAT(DISTINCT game_slug) AS game_slugs
             FROM source_attribution
            GROUP BY source
            ORDER BY count DESC)SQL",
                                   {}, kAttributionListLimits);
        std::vector<SourceCount> out;
        for (const auto& r : rows) {
            SourceCount c;
            c.source = r.str("source");
            c.count = r.num("count");
            for (const auto& s : str::split(r.str("game_slugs"), ",")) {
                auto t = str::trim(s);
                if (!t.empty()) c.gameSlugs.push_back(std::move(t));
            }
            c.lastImportedAt = r.opt_num("last_imported_at");
            out.push_back(std::move(c));
        }
        return out;
    }

    void close() override { db_->close(); }

private:
    sql::DatabasePtr db_;
};

} // namespace

Json to_json(const Attribution& v) {
    return Json{{"utteranceId", v.utteranceId}, {"source", v.source}, {"sourceUrl", v.sourceUrl}, {"nativeId", v.nativeId},
                {"gameSlug", v.gameSlug ? Json(*v.gameSlug) : Json(nullptr)}, {"importedAt", v.importedAt}};
}

Json to_json(const SourceCount& v) {
    return Json{{"source", v.source}, {"count", v.count}, {"gameSlugs", v.gameSlugs},
                {"lastImportedAt", v.lastImportedAt ? Json(*v.lastImportedAt) : Json(nullptr)}};
}

std::string default_attribution_path() { return fs::join(core::resolve_active_kg_dir(config::get_config()), ".attribution.sqlite"); }

std::unique_ptr<AttributionStore> open_attribution_store(const std::string& db_path) {
    fs::ensure_dir(fs::dirname(fs::resolve(db_path)));
    auto db = sql::Database::open(db_path);
    db->pragma("journal_mode = WAL");
    db->exec(R"SQL(CREATE TABLE IF NOT EXISTS source_attribution (
       utterance_id TEXT PRIMARY KEY,
       source       TEXT NOT NULL,
       source_url   TEXT NOT NULL,
       native_id    TEXT NOT NULL,
       game_slug    TEXT,
       imported_at  INTEGER NOT NULL
     ))SQL");
    return std::make_unique<SqliteAttributionStore>(std::move(db));
}

} // namespace di::crawler::sources
