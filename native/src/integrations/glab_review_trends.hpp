#pragma once
// src/integrations/glab-review-trends.ts の移植: Di に取り込み済みの Steam レビュー記録を、GLAB 向けに
// ゲーム別の直近傾向へ縮約する。本文・投稿者は外へ出さない (匿名化済みの集計だけ)。
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.hpp"
#include "base/sqlite.hpp"
#include "crawler/sources/attribution_store.hpp"

namespace di::integrations {

inline constexpr std::int64_t kReviewWindowMs = 7LL * 24 * 60 * 60 * 1000;
inline constexpr double kMaxSourceRecords = 100'000;

struct StoredReviewSignal {
    std::optional<std::string> gameSlug;
    std::optional<std::int64_t> appId;
    double postedAt = 0;
    /** "positive" | "negative" | nullopt */
    std::optional<std::string> reaction;
    bool operator==(const StoredReviewSignal&) const = default;
};
Json to_json(const StoredReviewSignal& s);

struct GameReviewTrend {
    std::string gameSlug;
    std::optional<std::int64_t> appId;
    double recentReviewCount = 0;
    std::optional<double> positivePercent;
    std::string latestReviewAt;
};
/** TS と同じキー順 (gameSlug, appId, recentReviewCount, positivePercent, latestReviewAt)。null は null で出す。 */
Json to_json(const GameReviewTrend& t);

/** attribution の steam 記録を列挙する境界 (テストで差し替え可)。 */
using ListSteamAttributions = std::function<std::vector<crawler::sources::Attribution>()>;

/** attribution と active KG を結合し、集計に必要な匿名化済み信号だけを取り出す (除外済みは飛ばす)。 */
std::vector<StoredReviewSignal> read_stored_steam_review_signals(sql::Database& db, const ListSteamAttributions& list);
std::vector<StoredReviewSignal> read_stored_steam_review_signals(sql::Database& db, crawler::sources::AttributionStore& attribution);

/** Di に取り込み済みのレビュー記録だけを、直近 7 日のゲーム別傾向へ縮約する。 */
std::vector<GameReviewTrend> analyze_stored_review_trends(const std::vector<StoredReviewSignal>& records,
                                                          std::optional<std::int64_t> now_ms = std::nullopt, int limit = 8);

/** active KG の Steam 取得記録を読み、本文・投稿者を外へ出さない集計だけを返す。 */
std::vector<GameReviewTrend> load_stored_steam_review_trends(std::optional<std::int64_t> now_ms = std::nullopt, int limit = 8);

/** Steam collector が attribution に残した URL から取得対象の App ID を復元する (公式ホストの https のみ)。 */
std::optional<std::int64_t> steam_app_id_from_source_url(std::string_view value);

} // namespace di::integrations
