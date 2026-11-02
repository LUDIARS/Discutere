#pragma once
// niconico (ニコニコ動画) collector (Phase 1) — src/crawler/sources/niconico.ts の移植。
//
// Snapshot Search API v2 で人気動画を発見 → 各動画の watch ページから nvComment 設定を抽出
// → nvComment スレッド API でコメント (弾幕) を取得し ExternalUtterance に正規化する。
// API キー不要。map 系は純関数 (テスト用)、fetch 系がネットワーク I/O (同期)。
// 取得先は公開サービスなので allow_private_network=false (nvComment の server は watch ページ由来の外部値)。
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "crawler/sources/types.hpp"

namespace di::crawler::sources {

/** 取得元 (テストで偽サーバに差し替える)。 */
struct NicoEndpoints {
    std::string search_base = "https://snapshot.search.nicovideo.jp";
    std::string watch_base = "https://www.nicovideo.jp";
};

struct NicoVideoRef {
    std::string contentId;
    std::string title;
    std::optional<double> viewCounter;
    std::optional<double> commentCounter;
};

/** Snapshot Search API v2 で動画を viewCount 順に発見する。失敗は std::runtime_error。 */
std::vector<NicoVideoRef> search_nico_videos(const std::string& query, int limit, const FetchFn& fetch_impl = {},
                                             const NicoEndpoints& endpoints = {});

struct NvComment {
    std::string threadKey;
    std::string server;
    /** { targets: [{id, fork}], language? } — 取得時にそのまま送り返す。 */
    Json params = Json::object();
};

/** watch ページ HTML の <meta name="server-response" content="..."> から nvComment 設定を取り出す (純関数)。 */
std::optional<NvComment> extract_nv_comment(std::string_view html);

struct NicoComment {
    std::optional<std::string> id;
    std::optional<double> no;
    std::string body;
    std::optional<std::string> postedAt;
    std::optional<std::string> userId;
    std::optional<double> vposMs;
};
NicoComment nico_comment_from_json(const Json& j);

/** 1 動画の watch → nvComment → コメント取得。失敗は std::runtime_error。 */
std::vector<NicoComment> fetch_nico_comments(const std::string& content_id, const FetchFn& fetch_impl = {},
                                             const NicoEndpoints& endpoints = {});

/** niconico コメント 1 件 → ExternalUtterance (純関数)。 */
ExternalUtterance map_nico_comment(const std::string& content_id, const std::string& game_slug, const NicoComment& c);

struct NicoFetchOptions {
    std::string gameSlug;
    std::string query;
    /** 取得する動画数 (viewCount 上位)。既定 5 */
    std::optional<int> maxVideos;
    /** 動画あたりのコメント上限。既定 無制限 */
    std::optional<int> maxComments;
    std::optional<long long> politeDelayMs;
    FetchFn fetchImpl;
    NicoEndpoints endpoints;
    std::function<void(int videos, std::size_t comments)> onVideo;
};

/** 動画発見 → コメント収集を一括で行う。 */
std::vector<ExternalUtterance> fetch_niconico_discussions(const NicoFetchOptions& opts);

} // namespace di::crawler::sources
