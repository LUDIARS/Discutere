#pragma once
// source-attribution sidecar (src/crawler/sources/attribution-store.ts) — spec/feature/crawler/EXTERNAL-SOURCES.md §3 / §6。
//
// 露出制御で「ソース種別 + 元 URL を end user に開示」するため、取り込み時に utterance ごとの出所
// (source / source_url / native_id / game_slug) を引けるようにする。utterances 本体の schema は不変。
// 個人アンカー (authorId / authorName) はここに保持しない (§6 露出マスク対象なので別レイヤー)。
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/json.hpp"
#include "base/sqlite.hpp"

namespace di::crawler::sources {

struct Attribution {
    std::string utteranceId;
    std::string source;
    std::string sourceUrl;
    std::string nativeId;
    std::optional<std::string> gameSlug;  // null
    double importedAt = 0;
};
Json to_json(const Attribution& v);

/** source 別の集計 1 行 (データ調整 UX のバッジ・一覧用)。 */
struct SourceCount {
    std::string source;
    double count = 0;
    std::vector<std::string> gameSlugs;
    std::optional<double> lastImportedAt;  // null
};
Json to_json(const SourceCount& v);

struct AttributionInput {
    std::string utteranceId;
    std::string source;
    std::string sourceUrl;
    std::string nativeId;
    std::optional<std::string> gameSlug;
    std::optional<double> at;
};

/** 一覧・集計の行数・バイト数の上限 (DB の値も外部入力として課金する)。 */
inline constexpr sql::QueryLimits kAttributionListLimits{200'000, 256u << 20};

class AttributionStore {
public:
    virtual ~AttributionStore() = default;
    /** utteranceId に出所メタを紐付けて保存 (upsert)。 */
    virtual void set(const AttributionInput& input) = 0;
    /** utteranceId の出所メタを引く。無ければ nullopt (内部 Discord 発話など)。 */
    virtual std::optional<Attribution> get(const std::string& utterance_id) = 0;
    /** source (任意で gameSlug) に属する取り込み発話を新しい順に列挙する。 */
    virtual std::vector<Attribution> list_by_source(const std::string& source, const std::optional<std::string>& game_slug = std::nullopt,
                                                    double limit = 300) = 0;
    /** source 別の件数・対象ゲーム・直近取り込み時刻を集計する。 */
    virtual std::vector<SourceCount> counts_by_source() = 0;
    virtual void close() = 0;
};

/** active KG ディレクトリ配下の既定パス (§12 — KG 切替で帰属も切り替わる)。 */
std::string default_attribution_path();

/** 開く (親ディレクトリは作る)。失敗は sql::Error。 */
std::unique_ptr<AttributionStore> open_attribution_store(const std::string& db_path = default_attribution_path());

} // namespace di::crawler::sources
