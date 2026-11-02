#include "crawler/sources/youtube_videos.hpp"

#include <algorithm>
#include <stdexcept>

#include "base/strings.hpp"
#include "base/url.hpp"

namespace di::crawler::sources {

namespace {
constexpr std::size_t kMaxBytes = 4u << 20;
constexpr std::size_t kMaxPages = 1000;

Json get_json(const std::string& url, const FetchFn& fetch_impl) {
    http::ClientRequest req;
    req.url = url;
    req.max_response_bytes = kMaxBytes;
    auto res = do_fetch(fetch_impl, req);
    // URL には API キーが入るので、エラー文には載せない。
    if (!res.transport_ok) throw std::runtime_error("youtube: " + res.error);
    if (!res.ok()) throw std::runtime_error("youtube " + std::to_string(res.status) + ": " + str::js_slice(res.body, 0, 200));
    JsonLimits limits;
    limits.max_bytes = kMaxBytes;
    auto data = parse_json(res.body, limits);
    if (!data || !data->is_object()) throw std::runtime_error("youtube: invalid JSON");
    return std::move(*data);
}

std::optional<double> parse_date(const Json& o, const char* key) {
    auto s = json_opt_string(o, key);
    if (!s || s->empty()) return std::nullopt;
    return js_date_parse(*s);
}
} // namespace

Json to_json(const VideoRef& v) {
    Json o = Json::object();
    o["videoId"] = v.videoId;
    o["title"] = v.title;
    if (v.channelId) o["channelId"] = *v.channelId;
    if (v.publishedAt) o["publishedAt"] = *v.publishedAt;
    return o;
}

std::optional<VideoRef> map_search_video(const Json& item) {
    if (!item.is_object()) return std::nullopt;
    auto video_id = json_opt_string(json_object(item, "id"), "videoId");
    if (!video_id || video_id->empty()) return std::nullopt;  // channel/playlist 結果は除外
    const Json& snippet = json_object(item, "snippet");
    VideoRef v;
    v.videoId = *video_id;
    v.title = json_string(snippet, "title");
    v.channelId = json_opt_string(snippet, "channelId");
    v.publishedAt = parse_date(snippet, "publishedAt");
    return v;
}

std::optional<VideoRef> map_playlist_video(const Json& item) {
    if (!item.is_object()) return std::nullopt;
    const Json& details = json_object(item, "contentDetails");
    const Json& snippet = json_object(item, "snippet");
    auto video_id = json_opt_string(details, "videoId");
    if (!video_id) video_id = json_opt_string(json_object(snippet, "resourceId"), "videoId");
    if (!video_id || video_id->empty()) return std::nullopt;
    VideoRef v;
    v.videoId = *video_id;
    v.title = json_string(snippet, "title");
    v.channelId = json_opt_string(snippet, "videoOwnerChannelId");
    v.publishedAt = parse_date(details, "videoPublishedAt");
    return v;
}

std::vector<VideoRef> discover_videos_by_search(const SearchDiscoverOptions& opts) {
    const std::size_t max = opts.maxResults.value_or(50);
    const std::string order = opts.order.value_or("relevance");
    std::vector<VideoRef> out;
    std::optional<std::string> page_token;
    // ページ数にも上限を置く (quota を渡さない呼び出しで空ページが続く異常応答を止める。移植で追加)。
    for (std::size_t page = 0; page < kMaxPages && out.size() < max; ++page) {
        if (opts.quota && !opts.quota->can_spend(YT_COST.search)) break;
        if (opts.quota) opts.quota->spend(YT_COST.search, "search.list");
        const std::size_t per = std::min<std::size_t>(50, max - out.size());
        const std::string url = opts.apiBase + "/search?part=snippet&type=video&maxResults=" + std::to_string(per) + "&order=" +
                                url::encode_component(order) + "&q=" + url::encode_component(opts.query) +
                                (page_token ? "&pageToken=" + url::encode_component(*page_token) : std::string()) + "&key=" +
                                url::encode_component(opts.apiKey);
        const Json data = get_json(url, opts.fetchImpl);
        for (const auto& item : json_array(data, "items"))
            if (auto ref = map_search_video(item)) out.push_back(std::move(*ref));
        page_token = json_opt_string(data, "nextPageToken");
        if (!page_token || page_token->empty()) break;
    }
    if (out.size() > max) out.resize(max);
    return out;
}

std::vector<VideoRef> discover_videos_by_channel(const ChannelDiscoverOptions& opts) {
    const std::size_t max = opts.maxResults.value_or(200);
    if (opts.quota) opts.quota->spend(YT_COST.list, "channels.list");
    const Json ch = get_json(opts.apiBase + "/channels?part=contentDetails&id=" + url::encode_component(opts.channelId) + "&key=" +
                                 url::encode_component(opts.apiKey),
                             opts.fetchImpl);
    std::optional<std::string> uploads;
    const Json& items = json_array(ch, "items");
    if (!items.empty())
        uploads = json_opt_string(json_object(json_object(items[0], "contentDetails"), "relatedPlaylists"), "uploads");
    if (!uploads || uploads->empty()) return {};

    std::vector<VideoRef> out;
    std::optional<std::string> page_token;
    for (std::size_t page = 0; page < kMaxPages && out.size() < max; ++page) {
        if (opts.quota && !opts.quota->can_spend(YT_COST.list)) break;
        if (opts.quota) opts.quota->spend(YT_COST.list, "playlistItems.list");
        const std::size_t per = std::min<std::size_t>(50, max - out.size());
        const std::string url = opts.apiBase + "/playlistItems?part=snippet,contentDetails&maxResults=" + std::to_string(per) +
                                "&playlistId=" + url::encode_component(*uploads) +
                                (page_token ? "&pageToken=" + url::encode_component(*page_token) : std::string()) + "&key=" +
                                url::encode_component(opts.apiKey);
        const Json data = get_json(url, opts.fetchImpl);
        for (const auto& item : json_array(data, "items"))
            if (auto ref = map_playlist_video(item)) out.push_back(std::move(*ref));
        page_token = json_opt_string(data, "nextPageToken");
        if (!page_token || page_token->empty()) break;
    }
    if (out.size() > max) out.resize(max);
    return out;
}

} // namespace di::crawler::sources
