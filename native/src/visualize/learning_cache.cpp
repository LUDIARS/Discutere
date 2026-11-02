#include "visualize/learning_cache.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include "analysis/gap_rate.hpp"
#include "base/fsutil.hpp"
#include "base/strings.hpp"
#include "base/time.hpp"
#include "visualize/conclusions.hpp"
#include "visualize/data_sources.hpp"
#include "visualize/learning_layers.hpp"

namespace di::visualize {

namespace {

/** ビューア HTML が投げるクエリと同じ上限値で焼く。 */
constexpr std::int64_t CONCLUSION_LIMIT = 200;
constexpr double LAYER_LIMIT = 80;
constexpr double LAYER_DETAIL_LIMIT = 8;
constexpr const char* META_KEY = "__meta__";
const JsonLimits kPayloadLimits{kLearningCachePayloadMaxBytes, 4'000'000, 64};

std::string path_or_default(const std::optional<std::string>& p) { return p ? *p : default_learning_cache_path(); }

sql::DatabasePtr open_db(const std::string& db_path) {
    fs::ensure_dir(fs::dirname(db_path));
    auto db = sql::Database::open(db_path);
    db->pragma("journal_mode = WAL");
    db->exec("CREATE TABLE IF NOT EXISTS cache (key TEXT PRIMARY KEY, payload TEXT NOT NULL, built_at INTEGER NOT NULL)");
    return db;
}

constexpr const char* kPutSql =
    "INSERT INTO cache (key, payload, built_at) VALUES (?, ?, ?) "
    "ON CONFLICT(key) DO UPDATE SET payload = excluded.payload, built_at = excluded.built_at";

/** Math.round (0.5 は +∞ 方向)。 */
double js_round(double x) { return std::floor(x + 0.5); }

} // namespace

std::string default_learning_cache_path() { return fs::resolve("./data/learning-cache.sqlite"); }

Json to_json(const BuildLearningCacheResult& r) {
    Json o = Json::object();
    o["path"] = r.path;
    o["builtAt"] = r.builtAt;
    o["conclusions"] = r.conclusions;
    o["details"] = r.details;
    o["layers"] = r.layers;
    o["sources"] = r.sources;
    return o;
}

Json to_json(const GapSummaryEntry& e) {
    Json o = Json::object();
    o["slug"] = e.slug;
    o["game"] = e.game;
    o["total"] = e.total;
    o["coverage"] = e.coverage;
    o["gapRate"] = e.gapRate;
    o["negativeRate"] = e.negativeRate;
    o["topGapMechanic"] = e.topGapMechanic ? Json(*e.topGapMechanic) : Json(nullptr);
    return o;
}

BuildLearningCacheResult build_learning_cache(core::Core& core, const std::string& workspace_id, const std::optional<std::string>& db_path,
                                              std::optional<std::int64_t> now_opt) {
    const std::string path = path_or_default(db_path);
    const std::int64_t now = now_opt ? *now_opt : time::now_ms();
    auto db = open_db(path);
    BuildLearningCacheResult out;
    out.path = path;
    out.builtAt = now;
    try {
        auto put = [&](const std::string& key, const Json& value) { db->run(kPutSql, {key, json_stringify(value), now}); };
        db->transaction([&] {
            // 結論詳細は件数が変動するので毎回作り直す (古い gap の残骸を残さない)。
            db->run("DELETE FROM cache WHERE key LIKE 'conclusion:%'");

            const auto conclusions = list_conclusions(core, workspace_id, CONCLUSION_LIMIT);
            Json list = Json::array();
            for (const auto& c : conclusions) list.push_back(to_json(c));
            put("conclusions", Json{{"conclusions", list}});

            std::int64_t details = 0;
            for (const auto& c : conclusions) {
                const auto detail = get_conclusion_detail(core, workspace_id, c.gapId);
                if (detail) {
                    put("conclusion:" + c.gapId, to_json(*detail));
                    details += 1;
                }
            }

            std::int64_t layers = 0;
            for (const char* layer : {"knowledge", "games", "opinions"}) {
                LearningLayerOptions opts;
                opts.limit = LAYER_LIMIT;
                opts.detailLimit = LAYER_DETAIL_LIMIT;
                put(std::string("layer:") + layer, build_learning_layer_snapshot(core.raw(), workspace_id, layer, opts));
                layers += 1;
            }

            // データソース構成 (どの取得元が何件か) を焼く。
            const Json data_sources = build_data_source_snapshot(core.raw(), workspace_id, now);
            put("data-sources", data_sources);

            put(META_KEY, Json{{"builtAt", now}, {"workspaceId", workspace_id}});
            out.conclusions = static_cast<std::int64_t>(conclusions.size());
            out.details = details;
            out.layers = layers;
            out.sources = data_sources.contains("sources") && data_sources["sources"].is_array()
                              ? static_cast<std::int64_t>(data_sources["sources"].size())
                              : 0;
        });
    } catch (...) {
        db->close();
        throw;
    }
    db->close();
    return out;
}

BuildGapCacheResult build_gap_cache(const std::optional<std::string>& db_path, const std::optional<std::string>& games_dir_opt,
                                    const std::optional<std::string>& kuzu_path, std::optional<std::int64_t> now_opt) {
    const std::string path = path_or_default(db_path);
    const std::string games_dir = games_dir_opt ? *games_dir_opt : fs::resolve("./data/games");
    const std::int64_t now = now_opt ? *now_opt : time::now_ms();
    auto db = open_db(path);
    BuildGapCacheResult out;
    try {
        std::vector<std::string> slugs;
        if (fs::exists(games_dir))
            for (const auto& f : fs::list_dir(games_dir))
                if (str::ends_with(f, ".md")) slugs.push_back(f.substr(0, f.size() - 3));
        std::vector<GapSummaryEntry> summary;
        db->run("DELETE FROM cache WHERE key LIKE 'gap:%'");
        for (const auto& slug : slugs) {
            analysis::GapRateReport report;
            try {
                report = analysis::analyze_gap_rate(slug, games_dir, kuzu_path);
            } catch (...) {
                continue;  // md 不正 / 該当発話なし等は skip
            }
            if (report.total == 0) continue;
            // 運営想定 (intended_aspects) を 1 つも定義していない md は Gap 比較に乗せない。
            const bool enriched = std::any_of(report.byMechanic.begin(), report.byMechanic.end(),
                                              [](const auto& m) { return !m.intendedAspects.empty(); });
            if (!enriched) continue;
            db->run(kPutSql, {"gap:" + slug, json_stringify(analysis::to_json(report)), now});
            GapSummaryEntry e;
            e.slug = slug;
            e.game = report.game;
            e.total = report.total;
            e.coverage = js_round((1 - report.neutralRate) * 1000) / 1000;
            e.gapRate = report.gapRate;
            e.negativeRate = report.negativeRate;
            for (const auto& m : report.byMechanic)
                if (m.attributed > 0) {
                    e.topGapMechanic = m.name;
                    break;
                }
            summary.push_back(std::move(e));
        }
        std::stable_sort(summary.begin(), summary.end(), [](const auto& a, const auto& b) { return b.gapRate < a.gapRate; });
        Json games = Json::array();
        for (const auto& e : summary) games.push_back(to_json(e));
        db->run(kPutSql, {"gap-summary", json_stringify(Json{{"games", games}}), now});
        out.games = static_cast<std::int64_t>(summary.size());
    } catch (...) {
        db->close();
        throw;
    }
    db->close();
    return out;
}

std::optional<Json> LearningCacheReader::read(const std::string& key) {
    const auto row = db_->get("SELECT payload FROM cache WHERE key = ?", {key});
    if (!row) return std::nullopt;
    const auto& v = (*row)["payload"];
    if (!v.is_text()) return std::nullopt;
    // payload は外部入力として上限つきで読む (TS は JSON.parse のまま。壊れた行は無いものとして扱う)。
    return parse_json(v.as_string(), kPayloadLimits);
}

Json LearningCacheReader::conclusions() {
    auto v = read("conclusions");
    return v ? *v : Json{{"conclusions", Json::array()}};
}
std::optional<Json> LearningCacheReader::conclusion(const std::string& gap_id) { return read("conclusion:" + gap_id); }
std::optional<Json> LearningCacheReader::layer(const std::string& name) { return read("layer:" + name); }
std::optional<Json> LearningCacheReader::gap(const std::string& slug) { return read("gap:" + slug); }
Json LearningCacheReader::gap_summary() {
    auto v = read("gap-summary");
    return v ? *v : Json{{"games", Json::array()}};
}
Json LearningCacheReader::data_sources() {
    auto v = read("data-sources");
    if (v) return *v;
    Json o = Json::object();
    o["totals"] = Json{{"sources", 0}};
    o["sources"] = Json::array();
    o["byGame"] = Json::array();
    return o;
}
std::optional<std::int64_t> LearningCacheReader::built_at() {
    const auto meta = read(META_KEY);
    if (!meta || !meta->is_object() || !meta->contains("builtAt") || !(*meta)["builtAt"].is_number()) return std::nullopt;
    return static_cast<std::int64_t>((*meta)["builtAt"].get<double>());
}
void LearningCacheReader::close() { db_->close(); }

std::unique_ptr<LearningCacheReader> open_learning_cache_reader(const std::optional<std::string>& db_path) {
    sql::OpenOptions o;
    o.readonly = true;
    o.create = false;
    o.fileMustExist = true;
    return std::make_unique<LearningCacheReader>(sql::Database::open(path_or_default(db_path), o));
}

bool learning_cache_exists(const std::optional<std::string>& db_path) { return fs::exists(path_or_default(db_path)); }

} // namespace di::visualize
