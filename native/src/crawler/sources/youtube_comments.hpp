#pragma once
// YouTube コメントクローラ — spec/feature/crawler/EXTERNAL-SOURCES.md §4.2(b) (src/crawler/sources/youtube-comments.ts)。
//
// 動画 id → コメント本文。commentThreads.list (1 unit/100 thread) でトップレベル +
// 一部返信を取得し、ExternalUtterance に正規化する (返信は responds_to=トップコメント)。
// コメント無効動画は空配列を返して skip。API キーはログ・エラー文に出さない。
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "crawler/sources/types.hpp"
#include "crawler/sources/youtube_quota.hpp"

namespace di::crawler::sources {

/** YouTube Data API v3 の base (テストで差し替える)。 */
inline constexpr const char* YOUTUBE_API_BASE = "https://www.googleapis.com/youtube/v3";

struct YoutubeCommentRaw {
    std::string id;
    struct Snippet {
        std::optional<std::string> textOriginal;
        std::optional<std::string> textDisplay;
        std::optional<std::string> authorDisplayName;
        std::optional<std::string> authorChannelId;  // authorChannelId.value
        std::optional<std::string> publishedAt;
        std::optional<double> likeCount;
    };
    std::optional<Snippet> snippet;
};
std::optional<YoutubeCommentRaw> youtube_comment_from_json(const Json& j);

/** YouTube comment 1 件 → ExternalUtterance (純関数)。 */
ExternalUtterance map_youtube_comment(const YoutubeCommentRaw& c, const std::string& video_id, const std::string& game_slug,
                                      const std::optional<std::string>& parent_native_id = std::nullopt);

/** commentThreads.list の 1 thread (JSON) → [トップコメント, ...返信] (親→子順)。 */
std::vector<ExternalUtterance> map_comment_thread(const Json& thread, const std::string& video_id, const std::string& game_slug);

struct YoutubeCommentsOptions {
    std::string videoId;
    std::string gameSlug;
    std::string apiKey;
    /** 取得コメント数の上限 (top+replies 合算)。0/未指定で無制限 */
    std::optional<std::size_t> maxComments;
    /** relevance | time。既定 relevance */
    std::optional<std::string> order;
    std::shared_ptr<QuotaTracker> quota;
    FetchFn fetchImpl;
    std::string apiBase = YOUTUBE_API_BASE;
    /** ページ数の上限 (移植で追加)。 */
    std::size_t maxPages = 10'000;
    std::function<void(std::size_t total)> onPage;
};

/** 動画 1 本のコメントを取得して ExternalUtterance[] にする。失敗は std::runtime_error。 */
std::vector<ExternalUtterance> fetch_video_comments(const YoutubeCommentsOptions& opts);

/** YouTube の 4xx/5xx 応答本文が「コメント無効」を示すか (/commentsDisabled|disabled comments/i)。 */
bool is_comments_disabled(std::string_view body);

} // namespace di::crawler::sources
