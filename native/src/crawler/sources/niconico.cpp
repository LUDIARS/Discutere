#include "crawler/sources/niconico.hpp"

#include <stdexcept>

#include "base/strings.hpp"
#include "base/url.hpp"

namespace di::crawler::sources {

namespace {
constexpr const char* USER_AGENT = "LUDIARS-Discutere-Crawler/0.1 (+https://github.com/LUDIARS)";
constexpr const char* SOURCE = "niconico";

http::ClientRequest base_request(std::string url, std::size_t max_bytes) {
    http::ClientRequest r;
    r.url = std::move(url);
    r.headers.set("User-Agent", USER_AGENT);
    r.timeout = std::chrono::milliseconds(30'000);
    r.max_response_bytes = max_bytes;
    r.allow_private_network = false;
    return r;
}

Json parse_or_throw(const std::string& body, const char* what, std::size_t max_bytes) {
    JsonLimits limits;
    limits.max_bytes = max_bytes;
    limits.max_elements = 1'000'000;
    std::string error;
    auto j = parse_json(body, limits, &error);
    if (!j) throw std::runtime_error(std::string(what) + ": invalid JSON (" + error + ")");
    return std::move(*j);
}

std::optional<double> opt_num(const Json& j, const char* k) {
    auto it = j.find(k);
    if (it != j.end() && it->is_number()) return it->get<double>();
    return std::nullopt;
}
} // namespace

std::vector<NicoVideoRef> search_nico_videos(const std::string& query, int limit, const FetchFn& fetch_impl,
                                             const NicoEndpoints& endpoints) {
    const std::string url = endpoints.search_base + "/api/v2/snapshot/video/contents/search?" + "q=" + url::encode_component(query) +
                            "&targets=title,description,tags" + "&fields=contentId,title,viewCounter,commentCounter&_sort=-viewCounter&_limit=" +
                            std::to_string(limit);
    const std::size_t max = 4u << 20;
    auto res = do_fetch(fetch_impl, base_request(url, max));
    if (!res.transport_ok) throw std::runtime_error("niconico search: " + res.error);
    if (!res.ok()) throw std::runtime_error("niconico search HTTP " + std::to_string(res.status));
    const Json data = parse_or_throw(res.body, "niconico search", max);
    std::vector<NicoVideoRef> out;
    if (!data.is_object() || !data.contains("data") || !data["data"].is_array()) return out;
    for (const auto& v : data["data"]) {
        // contentId が文字列でないものは URL を組めないので捨てる (TS は検査せず使っていた)。
        if (!v.is_object() || !v.contains("contentId") || !v["contentId"].is_string()) continue;
        NicoVideoRef ref;
        ref.contentId = v["contentId"].get<std::string>();
        ref.title = json_string(v, "title");
        ref.viewCounter = opt_num(v, "viewCounter");
        ref.commentCounter = opt_num(v, "commentCounter");
        out.push_back(std::move(ref));
    }
    return out;
}

std::optional<NvComment> extract_nv_comment(std::string_view html) {
    // /<meta name="server-response" content="([^"]+)"/ (線形)
    static constexpr std::string_view marker = "<meta name=\"server-response\" content=\"";
    std::size_t from = 0;
    std::string_view raw;
    while (true) {
        const auto at = html.find(marker, from);
        if (at == std::string_view::npos) return std::nullopt;
        const auto start = at + marker.size();
        const auto end = html.find('"', start);
        if (end == std::string_view::npos) return std::nullopt;
        if (end > start) {
            raw = html.substr(start, end - start);
            break;
        }
        from = at + 1;  // 空の content は [^"]+ に一致しないので次を探す
    }
    // 置換の順序は TS と同じ (&quot; → &amp; → &lt; → &gt; → &#39;)。
    std::string json = str::replace_all(raw, "&quot;", "\"");
    json = str::replace_all(json, "&amp;", "&");
    json = str::replace_all(json, "&lt;", "<");
    json = str::replace_all(json, "&gt;", ">");
    json = str::replace_all(json, "&#39;", "'");
    JsonLimits limits;
    limits.max_bytes = 8u << 20;
    limits.max_elements = 1'000'000;
    auto sr = parse_json(json, limits);
    if (!sr || !sr->is_object()) return std::nullopt;
    const Json* node = &*sr;
    for (const char* k : {"data", "response", "comment", "nvComment"}) {
        if (!node->is_object() || !node->contains(k)) return std::nullopt;
        node = &(*node)[k];
    }
    if (!node->is_object()) return std::nullopt;
    NvComment nv;
    nv.threadKey = json_string(*node, "threadKey");
    nv.server = json_string(*node, "server");
    if (node->contains("params")) nv.params = (*node)["params"];
    return nv;
}

NicoComment nico_comment_from_json(const Json& j) {
    NicoComment c;
    if (!j.is_object()) return c;
    c.id = json_opt_string(j, "id");
    c.no = opt_num(j, "no");
    c.body = json_string(j, "body");
    c.postedAt = json_opt_string(j, "postedAt");
    c.userId = json_opt_string(j, "userId");
    c.vposMs = opt_num(j, "vposMs");
    return c;
}

std::vector<NicoComment> fetch_nico_comments(const std::string& content_id, const FetchFn& fetch_impl, const NicoEndpoints& endpoints) {
    const std::size_t page_max = 8u << 20;
    auto watch_req = base_request(endpoints.watch_base + "/watch/" + url::encode_component(content_id), page_max);
    watch_req.headers.set("Accept-Language", "ja");
    auto watch = do_fetch(fetch_impl, watch_req);
    if (!watch.transport_ok) throw std::runtime_error("niconico watch: " + watch.error + " (" + content_id + ")");
    if (!watch.ok()) throw std::runtime_error("niconico watch HTTP " + std::to_string(watch.status) + " (" + content_id + ")");
    auto nv = extract_nv_comment(watch.body);
    if (!nv) return {};

    const std::size_t thread_max = 16u << 20;
    auto req = base_request(nv->server + "/v1/threads", thread_max);
    req.method = "POST";
    req.headers.set("Content-Type", "application/json");
    req.headers.set("x-frontend-id", "6");
    req.headers.set("x-frontend-version", "0");
    req.body = json_stringify(Json{{"threadKey", nv->threadKey}, {"params", nv->params}, {"additionals", Json::object()}});
    auto res = do_fetch(fetch_impl, req);
    if (!res.transport_ok) throw std::runtime_error("nvComment: " + res.error + " (" + content_id + ")");
    if (!res.ok()) throw std::runtime_error("nvComment HTTP " + std::to_string(res.status) + " (" + content_id + ")");
    const Json data = parse_or_throw(res.body, "nvComment", thread_max);
    std::vector<NicoComment> out;
    const Json* threads = nullptr;
    if (data.is_object() && data.contains("data") && data["data"].is_object() && data["data"].contains("threads") &&
        data["data"]["threads"].is_array())
        threads = &data["data"]["threads"];
    if (!threads || threads->empty()) return out;
    const Json* main = nullptr;
    for (const auto& t : *threads)
        if (t.is_object() && t.contains("fork") && t["fork"] == "main") {
            main = &t;
            break;
        }
    if (!main) main = &(*threads)[0];
    if (main->is_object() && main->contains("comments") && !(*main)["comments"].is_null()) {
        if ((*main)["comments"].is_array())
            for (const auto& c : (*main)["comments"]) out.push_back(nico_comment_from_json(c));
        return out;
    }
    for (const auto& t : *threads)
        if (t.is_object() && t.contains("comments") && t["comments"].is_array())
            for (const auto& c : t["comments"]) out.push_back(nico_comment_from_json(c));
    return out;
}

ExternalUtterance map_nico_comment(const std::string& content_id, const std::string& game_slug, const NicoComment& c) {
    const std::string key = c.id ? *c.id : (c.no ? js_number_to_string(*c.no) : std::string());
    ExternalUtterance u;
    u.source = SOURCE;
    u.nativeId = content_id + ":" + key;
    u.gameSlug = game_slug;
    u.threadKey = content_id;  // 動画 = 議論の場 (session)
    u.content = c.body;
    u.lang = "ja";
    u.postedAt = c.postedAt && !c.postedAt->empty() ? js_date_parse(*c.postedAt) : 0;
    u.videoOffsetMs = c.vposMs;
    // userId は匿名化済みハッシュで公開・安定。取れない弾幕は動画内 no で擬似アンカー化
    u.authorId = c.userId && !c.userId->empty() ? "nico:" + *c.userId : "nico-anon:" + content_id + ":" + key;
    u.sourceUrl = "https://www.nicovideo.jp/watch/" + content_id;
    return u;
}

std::vector<ExternalUtterance> fetch_niconico_discussions(const NicoFetchOptions& opts) {
    const long long delay = opts.politeDelayMs.value_or(1500);
    const auto videos = search_nico_videos(opts.query, opts.maxVideos.value_or(5), opts.fetchImpl, opts.endpoints);
    std::vector<ExternalUtterance> out;
    int vi = 0;
    for (const auto& v : videos) {
        polite_sleep(delay);
        try {
            auto comments = fetch_nico_comments(v.contentId, opts.fetchImpl, opts.endpoints);
            std::size_t n = comments.size();
            if (opts.maxComments && *opts.maxComments > 0) n = std::min<std::size_t>(n, static_cast<std::size_t>(*opts.maxComments));
            for (std::size_t i = 0; i < n; ++i) {
                auto u = map_nico_comment(v.contentId, opts.gameSlug, comments[i]);
                if (!str::trim(u.content).empty()) out.push_back(std::move(u));
            }
        } catch (const std::exception&) {
            // 1 動画の失敗は致命にしない (非公開/削除/コメント不可)
        }
        vi += 1;
        if (opts.onVideo) opts.onVideo(vi, out.size());
    }
    return out;
}

} // namespace di::crawler::sources
