#pragma once
// YouTube 動画クローラ (discovery) — spec/feature/crawler/EXTERNAL-SOURCES.md §4.2(a) (src/crawler/sources/youtube-videos.ts)。
//
// ゲーム名 → コメントを取りに行く候補 動画 id リスト (VideoRef) を作る。
// quota 節約のため、チャンネルが分かるなら uploads playlist (1 unit/page) を優先し、
// キーワード検索 (search.list, 100 units/call) は呼び出し回数を上限化する。
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "crawler/sources/types.hpp"
#include "crawler/sources/youtube_comments.hpp"
#include "crawler/sources/youtube_quota.hpp"

namespace di::crawler::sources {

struct VideoRef {
    std::string videoId;
    std::string title;
    std::optional<std::string> channelId;
    std::optional<double> publishedAt;  // epoch ms
};
Json to_json(const VideoRef& v);

/** search.list の 1 item → VideoRef (videoId 無し = channel/playlist 結果は nullopt)。 */
std::optional<VideoRef> map_search_video(const Json& item);
/** playlistItems.list の 1 item → VideoRef。 */
std::optional<VideoRef> map_playlist_video(const Json& item);

struct YoutubeFetchDeps {
    std::string apiKey;
    FetchFn fetchImpl;
    std::shared_ptr<QuotaTracker> quota;
    std::string apiBase = YOUTUBE_API_BASE;
};

struct SearchDiscoverOptions : YoutubeFetchDeps {
    std::string query;
    /** 取得する動画数の上限 (1 call=50件)。既定 50 */
    std::optional<std::size_t> maxResults;
    /** relevance | date | viewCount。既定 relevance */
    std::optional<std::string> order;
};

/** キーワード検索で動画を探す (search.list, 100 units/call)。失敗は std::runtime_error。 */
std::vector<VideoRef> discover_videos_by_search(const SearchDiscoverOptions& opts);

struct ChannelDiscoverOptions : YoutubeFetchDeps {
    std::string channelId;
    std::optional<std::size_t> maxResults;  // 既定 200
};

/** チャンネルの uploads playlist から動画を列挙する (channels.list 1u + playlistItems 1u/page)。 */
std::vector<VideoRef> discover_videos_by_channel(const ChannelDiscoverOptions& opts);

} // namespace di::crawler::sources
