#pragma once
// src/visualize/learning-cache.ts の移植: Learning View 読み取りキャッシュ。
// 学習ビューア (/learning*) が参照する各クエリ結果を、KG (Core) から 1 回だけ実行して
// 別 SQLite (data/learning-cache.sqlite) に JSON で固める。ビューア側はこのキャッシュだけを読む。
// 「収束した論述データは一度作ったら変わらない」前提で、データ追加・議論収束の後に作り直す運用。
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "base/json.hpp"
#include "base/sqlite.hpp"
#include "core/index.hpp"

namespace di::visualize {

/** path.resolve("./data/learning-cache.sqlite") (呼んだ時点の cwd で解決)。 */
std::string default_learning_cache_path();

/** キャッシュの 1 値 (JSON 文字列) を読み戻すときの上限。 */
inline constexpr std::size_t kLearningCachePayloadMaxBytes = 64u << 20;

struct BuildLearningCacheResult {
    std::string path;
    std::int64_t builtAt = 0;
    std::int64_t conclusions = 0;
    std::int64_t details = 0;
    std::int64_t layers = 0;
    std::int64_t sources = 0;
};
Json to_json(const BuildLearningCacheResult& r);

/** KG から全クエリを実行してキャッシュ SQLite を作り直す。db_path 省略は既定パス、now 省略は現在時刻。 */
BuildLearningCacheResult build_learning_cache(core::Core& core, const std::string& workspace_id,
                                              const std::optional<std::string>& db_path = std::nullopt,
                                              std::optional<std::int64_t> now = std::nullopt);

struct GapSummaryEntry {
    std::string slug;
    std::string game;
    double total = 0;
    /** 感情シグナル率 (= 1 - 中立率)。 */
    double coverage = 0;
    double gapRate = 0;
    double negativeRate = 0;
    std::optional<std::string> topGapMechanic;
};
Json to_json(const GapSummaryEntry& e);

struct BuildGapCacheResult {
    std::int64_t games = 0;
};

/**
 * data/games/<slug>.md (運営の想定感情の定義) を走査して各ゲームの Gap率を算出し cache へ焼く。
 * 観測は KG を read-only で読む。learning-cache と同じ SQLite に gap:<slug> / gap-summary を追記。
 */
BuildGapCacheResult build_gap_cache(const std::optional<std::string>& db_path = std::nullopt,
                                    const std::optional<std::string>& games_dir = std::nullopt,
                                    const std::optional<std::string>& kuzu_path = std::nullopt,
                                    std::optional<std::int64_t> now = std::nullopt);

/** 読み取り専用のキャッシュ (KG には一切触れない)。payload は上限つきで読み、壊れていれば nullopt。 */
class LearningCacheReader {
public:
    explicit LearningCacheReader(sql::DatabasePtr db) : db_(std::move(db)) {}
    Json conclusions();
    std::optional<Json> conclusion(const std::string& gap_id);
    std::optional<Json> layer(const std::string& name);
    std::optional<Json> gap(const std::string& slug);
    Json gap_summary();
    Json data_sources();
    std::optional<std::int64_t> built_at();
    void close();

private:
    std::optional<Json> read(const std::string& key);
    sql::DatabasePtr db_;
};

/** 読み取り専用でキャッシュを開く (無ければ sql::Error)。 */
std::unique_ptr<LearningCacheReader> open_learning_cache_reader(const std::optional<std::string>& db_path = std::nullopt);

bool learning_cache_exists(const std::optional<std::string>& db_path = std::nullopt);

} // namespace di::visualize
