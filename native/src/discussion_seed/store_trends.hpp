#pragma once
// ストアトレンド調査シード (#65)。移植元 src/discussion-seed/store-trends.ts。
// Steam の公開 featuredcategories から「トップセラー」を取得し、議論の種に変換する。
// API キー不要。取得失敗時は静的フォールバックで止めない。
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "discussion_seed/genres.hpp"

namespace di::discussion_seed {

inline constexpr const char* STEAM_FEATURED_URL = "https://store.steampowered.com/api/featuredcategories/?cc=jp&l=japanese";
/** 応答本文の上限 (外部 HTTP 応答の課金)。 */
inline constexpr std::size_t kSteamFeaturedMaxBytes = 4u << 20;

/** 取得先 (テストで偽サーバへ向けるための注入点)。 */
struct StoreTrendSource {
    std::string url = STEAM_FEATURED_URL;
    std::chrono::milliseconds timeout{8000};
    /** テストの偽サーバ (loopback) 用。本番は false のまま (SSRF 対策)。 */
    bool allow_private_network = false;
};

/** Steam トップセラーの作品名を取得 (失敗時は空)。limit の扱いは TS と同じ (push 後に判定)。 */
std::vector<std::string> fetch_steam_top_seller_names(std::int64_t limit, const StoreTrendSource& source = {});

/** ストアトレンドの種を取得する。Steam 取得が空ならフォールバックを使う。 */
std::vector<SeedTopic> fetch_store_trend_topics(std::int64_t limit = 5, const StoreTrendSource& source = {});

} // namespace di::discussion_seed
