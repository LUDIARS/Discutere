#include "crawler/sources/youtube_comments.hpp"

#include <stdexcept>

#include "base/strings.hpp"
#include "base/url.hpp"

namespace di::crawler::sources {

namespace {
constexpr const char* SOURCE = "youtube";
constexpr std::size_t kMaxBytes = 8u << 20;
} // namespace

std::optional<YoutubeCommentRaw> youtube_comment_from_json(const Json& j) {
    if (!j.is_object() || !j.contains("id") || !j["id"].is_string()) return std::nullopt;
    YoutubeCommentRaw c;
    c.id = j["id"].get<std::string>();
    if (j.contains("snippet") && j["snippet"].is_object()) {
        const Json& s = j["snippet"];
        YoutubeCommentRaw::Snippet sn;
        sn.textOriginal = json_opt_string(s, "textOriginal");
        sn.textDisplay = json_opt_string(s, "textDisplay");
        sn.authorDisplayName = json_opt_string(s, "authorDisplayName");
        if (s.contains("authorChannelId") && s["authorChannelId"].is_object())
            sn.authorChannelId = json_opt_string(s["authorChannelId"], "value");
        sn.publishedAt = json_opt_string(s, "publishedAt");
        sn.likeCount = json_opt_number(s, "likeCount");
        c.snippet = sn;
    }
    return c;
}

ExternalUtterance map_youtube_comment(const YoutubeCommentRaw& c, const std::string& video_id, const std::string& game_slug,
                                      const std::optional<std::string>& parent_native_id) {
    const auto* s = c.snippet ? &*c.snippet : nullptr;
    ExternalUtterance u;
    u.source = SOURCE;
    u.nativeId = c.id;
    u.gameSlug = game_slug;
    u.threadKey = video_id;
    // API-key reads of other users' comments expose textDisplay, not author-only textOriginal.
    u.content = s && s->textOriginal ? *s->textOriginal : (s && s->textDisplay ? *s->textDisplay : std::string());
    u.postedAt = s && s->publishedAt && !s->publishedAt->empty() ? js_date_parse(*s->publishedAt) : 0;
    u.authorId = s && s->authorChannelId ? *s->authorChannelId : std::string("unknown");  // channelId(UCxxxx) = 公開・安定アンカー
    if (s) u.authorName = s->authorDisplayName;
    u.parentNativeId = parent_native_id;
    u.signal = ExternalSignal{std::nullopt, s ? s->likeCount : std::nullopt};
    u.sourceUrl = "https://www.youtube.com/watch?v=" + video_id + "&lc=" + c.id;
    return u;
}

std::vector<ExternalUtterance> map_comment_thread(const Json& thread, const std::string& video_id, const std::string& game_slug) {
    std::vector<ExternalUtterance> out;
    if (!thread.is_object()) return out;
    const Json& snippet = json_object(thread, "snippet");
    if (!snippet.contains("topLevelComment")) return out;
    auto top = youtube_comment_from_json(snippet["topLevelComment"]);
    if (!top) return out;
    out.push_back(map_youtube_comment(*top, video_id, game_slug));
    for (const auto& r : json_array(json_object(thread, "replies"), "comments")) {
        if (auto reply = youtube_comment_from_json(r)) out.push_back(map_youtube_comment(*reply, video_id, game_slug, top->id));
    }
    return out;
}

bool is_comments_disabled(std::string_view body) {
    const std::string lower = str::to_lower_ascii(body);
    return str::contains(lower, "commentsdisabled") || str::contains(lower, "disabled comments");
}

std::vector<ExternalUtterance> fetch_video_comments(const YoutubeCommentsOptions& opts) {
    const std::string order = opts.order.value_or("relevance");
    const std::size_t max = opts.maxComments.value_or(0);
    std::vector<ExternalUtterance> out;
    std::optional<std::string> page_token;
    for (std::size_t page = 0; page < opts.maxPages; ++page) {
        if (opts.quota && !opts.quota->can_spend(YT_COST.list)) break;
        http::ClientRequest req;
        req.url = opts.apiBase + "/commentThreads?part=snippet,replies&maxResults=100&textFormat=plainText" + "&order=" +
                  url::encode_component(order) + "&videoId=" + url::encode_component(opts.videoId) +
                  (page_token ? "&pageToken=" + url::encode_component(*page_token) : std::string()) + "&key=" +
                  url::encode_component(opts.apiKey);
        req.max_response_bytes = kMaxBytes;
        auto res = do_fetch(opts.fetchImpl, req);
        if (!res.transport_ok) throw std::runtime_error("youtube commentThreads: " + res.error);
        if (!res.ok()) {
            if (res.status == 403 && is_comments_disabled(res.body)) return out;  // コメント無効 → skip
            throw std::runtime_error("youtube commentThreads " + std::to_string(res.status) + ": " + str::js_slice(res.body, 0, 200));
        }
        if (opts.quota) opts.quota->spend(YT_COST.list, "commentThreads.list");
        JsonLimits limits;
        limits.max_bytes = kMaxBytes;
        auto data = parse_json(res.body, limits);
        if (!data || !data->is_object()) throw std::runtime_error("youtube commentThreads: invalid JSON");

        for (const auto& thread : json_array(*data, "items")) {
            for (auto& u : map_comment_thread(thread, opts.videoId, opts.gameSlug)) {
                out.push_back(std::move(u));
                if (max && out.size() >= max) break;
            }
            if (max && out.size() >= max) break;
        }
        if (opts.onPage) opts.onPage(out.size());
        if (max && out.size() >= max) break;

        page_token = json_opt_string(*data, "nextPageToken");
        if (!page_token || page_token->empty()) break;
    }
    if (max && out.size() > max) out.resize(max);
    return out;
}

} // namespace di::crawler::sources
