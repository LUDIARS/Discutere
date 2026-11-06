#pragma once
// Steam レビュー collector (Phase 1) — spec/feature/crawler/EXTERNAL-SOURCES.md §4.1
// (src/crawler/sources/steam.ts の移植)。
//
// 公開エンドポイント `store.steampowered.com/appreviews/{appid}?json=1` を
// cursor ページングで全件取得し、ExternalUtterance に正規化する。API キー不要。
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "crawler/sources/types.hpp"

namespace di::crawler::sources {

struct SteamReviewAuthor {
    std::optional<std::string> steamid;
    std::optional<double> num_games_owned;
    std::optional<double> playtime_forever;
};

struct SteamReviewRaw {
    std::string recommendationid;
    std::optional<SteamReviewAuthor> author;
    std::string review;
    std::optional<std::string> language;
    double timestamp_created = 0;  // epoch seconds
    bool voted_up = false;
    std::optional<double> votes_up;
};
/** API の 1 件を読む (recommendationid が文字列でなければ nullopt)。 */
std::optional<SteamReviewRaw> steam_review_from_json(const Json& j);

/** Steam review 1 件 → ExternalUtterance (純関数)。 */
ExternalUtterance map_steam_review(long long app_id, const std::string& game_slug, const SteamReviewRaw& raw);

struct SteamFetchOptions {
    long long appId = 0;
    std::string gameSlug;
    /** Steam language フィルタ。既定 ["all"]。 */
    std::vector<std::string> languages;
    /** "recent" | "all" | "updated"。既定 "recent" */
    std::optional<std::string> filter;
    /** 取得上限。0/未指定で無制限 (全件) */
    std::optional<std::size_t> maxReviews;
    /** 同一ドメインへの最小間隔 ms。既定 1200 */
    std::optional<long long> politeDelayMs;
    FetchFn fetchImpl;
    /** 取得元 (テストで差し替え)。 */
    std::string base = "https://store.steampowered.com";
    /** ページ数の上限 (cursor が進み続ける異常応答で止まらないのを防ぐ。移植で追加)。 */
    std::size_t maxPages = 10'000;
    /** 1 ページ取得ごとの進捗コールバック (累計件数) */
    std::function<void(std::size_t total)> onPage;
};

/** Steam レビューを cursor 全件 (or maxReviews まで) 取得する。失敗は std::runtime_error。 */
std::vector<ExternalUtterance> fetch_steam_reviews(const SteamFetchOptions& opts);

} // namespace di::crawler::sources
