#include "crawler/sources/reddit.hpp"

#include <algorithm>
#include <deque>
#include <stdexcept>

#include "base/crypto.hpp"
#include "base/strings.hpp"
#include "base/url.hpp"

namespace di::crawler::sources {

namespace {
constexpr const char* SOURCE = "reddit";
constexpr std::size_t kMaxBytes = 8u << 20;

std::optional<double> opt_num(const Json& j, const char* k) {
    auto it = j.find(k);
    if (it != j.end() && it->is_number()) return it->get<double>();
    return std::nullopt;
}

/** 削除済 / 空コメントは取り込まない。 */
bool is_droppable_body(const std::optional<std::string>& body) {
    if (!body || body->empty()) return true;
    const auto t = str::trim(*body);
    return t.empty() || t == "[deleted]" || t == "[removed]";
}

Json get_json(const std::string& url, const std::string& token, const std::string& user_agent, const FetchFn& fetch_impl) {
    http::ClientRequest req;
    req.url = url;
    req.headers.set("Authorization", "Bearer " + token);
    req.headers.set("User-Agent", user_agent);
    req.max_response_bytes = kMaxBytes;
    auto res = do_fetch(fetch_impl, req);
    if (!res.transport_ok) throw std::runtime_error("reddit: " + res.error);
    if (!res.ok()) throw std::runtime_error("reddit " + std::to_string(res.status) + ": " + str::js_slice(res.body, 0, 200));
    JsonLimits limits;
    limits.max_bytes = kMaxBytes;
    limits.max_elements = 1'000'000;
    auto data = parse_json(res.body, limits);
    if (!data) throw std::runtime_error("reddit: invalid JSON");
    return std::move(*data);
}
} // namespace

RedditCommentRaw reddit_comment_from_json(const Json& j) {
    RedditCommentRaw r;
    if (!j.is_object()) return r;
    r.name = json_opt_string(j, "name");
    r.id = json_opt_string(j, "id");
    r.body = json_opt_string(j, "body");
    r.author = json_opt_string(j, "author");
    r.created_utc = opt_num(j, "created_utc");
    r.parent_id = json_opt_string(j, "parent_id");
    r.ups = opt_num(j, "ups");
    return r;
}

std::optional<ExternalUtterance> map_reddit_comment(const RedditCommentRaw& raw, const RedditContext& ctx) {
    if (is_droppable_body(raw.body)) return std::nullopt;
    const std::string native_id = raw.name ? *raw.name : (raw.id && !raw.id->empty() ? "t1_" + *raw.id : std::string());
    if (native_id.empty()) return std::nullopt;
    const std::string author = raw.author && !raw.author->empty() && *raw.author != "[deleted]" ? *raw.author : std::string("anonymous");
    ExternalUtterance u;
    u.source = SOURCE;
    u.nativeId = native_id;
    u.gameSlug = ctx.gameSlug;
    u.threadKey = ctx.submissionId;
    u.content = raw.body.value_or("");
    u.postedAt = raw.created_utc.value_or(0) * 1000;
    u.authorId = author;  // Reddit username = 公開・安定アンカー (§6)
    u.authorName = author;
    // 親が submission (t3_) ならスレッド直下 → responds_to 無し。t1_ なら親コメント。
    if (raw.parent_id && str::starts_with(*raw.parent_id, "t1_")) u.parentNativeId = raw.parent_id;
    u.signal = ExternalSignal{std::nullopt, raw.ups};
    if (ctx.permalink && !ctx.permalink->empty()) {
        std::string id = raw.id.value_or("");
        if (str::starts_with(id, "t1_")) id = id.substr(3);
        u.sourceUrl = "https://www.reddit.com" + *ctx.permalink + id + "/";
    } else {
        u.sourceUrl = "https://www.reddit.com/comments/" + ctx.submissionId + "/";
    }
    return u;
}

std::vector<ExternalUtterance> flatten_comments(const Json& children, const RedditContext& ctx, std::size_t max) {
    std::vector<ExternalUtterance> out;
    if (!children.is_array()) return out;
    // TS は stack.shift() + push (= 先頭から取り出す待ち行列)。同じ順序にする。
    std::deque<const Json*> queue;
    for (const auto& c : children) queue.push_back(&c);
    while (!queue.empty() && out.size() < max) {
        const Json* node = queue.front();
        queue.pop_front();
        if (!node->is_object() || !node->contains("kind") || (*node)["kind"] != "t1" || !node->contains("data") ||
            !(*node)["data"].is_object())
            continue;
        const Json& data = (*node)["data"];
        if (auto mapped = map_reddit_comment(reddit_comment_from_json(data), ctx)) out.push_back(std::move(*mapped));
        // 返信 (replies) は listing or ""。listing なら children を積む。
        if (data.contains("replies") && data["replies"].is_object()) {
            const Json& replies = data["replies"];
            if (replies.contains("data") && replies["data"].is_object() && replies["data"].contains("children") &&
                replies["data"]["children"].is_array())
                for (const auto& c : replies["data"]["children"]) queue.push_back(&c);
        }
    }
    return out;
}

std::string get_reddit_token(const RedditCredentials& creds, const FetchFn& fetch_impl, const RedditEndpoints& endpoints) {
    http::ClientRequest req;
    req.method = "POST";
    req.url = endpoints.token_url;
    req.headers.set("Authorization", "Basic " + crypto::base64_encode(creds.clientId + ":" + creds.clientSecret));
    req.headers.set("Content-Type", "application/x-www-form-urlencoded");
    req.headers.set("User-Agent", creds.userAgent);
    req.body = "grant_type=client_credentials";
    req.max_response_bytes = 64 * 1024;
    auto res = do_fetch(fetch_impl, req);
    if (!res.transport_ok) throw std::runtime_error("reddit token: " + res.error);
    if (!res.ok()) throw std::runtime_error("reddit token HTTP " + std::to_string(res.status));
    JsonLimits limits;
    limits.max_bytes = 64 * 1024;
    auto data = parse_json(res.body, limits);
    if (!data || !data->is_object()) throw std::runtime_error("reddit token: invalid JSON");
    auto token = json_opt_string(*data, "access_token");
    if (!token || token->empty()) throw std::runtime_error("reddit token: access_token 欠落");
    return *token;
}

std::vector<RedditThreadRef> search_threads(const RedditSearchOptions& opts) {
    const int limit = std::min(100, std::max(1, opts.limit.value_or(25)));
    const std::string sort = opts.sort.value_or("top");
    const std::string time = opts.time.value_or("year");
    const bool has_sub = opts.subreddit && !opts.subreddit->empty();
    const std::string path = has_sub ? "/r/" + url::encode_component(*opts.subreddit) + "/search" : std::string("/search");
    const std::string restrict = has_sub ? "&restrict_sr=1" : "";
    const std::string url = opts.endpoints.oauth_base + path + "?q=" + url::encode_component(opts.query) + "&sort=" +
                            url::encode_component(sort) + "&t=" + url::encode_component(time) + "&limit=" + std::to_string(limit) +
                            "&type=link" + restrict + "&raw_json=1";
    const Json data = get_json(url, opts.token, opts.userAgent, opts.fetchImpl);
    std::vector<RedditThreadRef> out;
    if (!data.is_object()) return out;
    const Json& children = json_array(json_object(data, "data"), "children");
    for (const auto& c : children) {
        if (!c.is_object() || !c.contains("data") || !c["data"].is_object()) continue;
        const Json& d = c["data"];
        if (!d.contains("id") || !d["id"].is_string()) continue;
        RedditThreadRef t;
        t.id = d["id"].get<std::string>();
        t.title = json_string(d, "title");
        t.subreddit = json_string(d, "subreddit");
        t.permalink = json_opt_string(d, "permalink").value_or("/comments/" + t.id + "/");
        t.numComments = opt_num(d, "num_comments").value_or(0);
        out.push_back(std::move(t));
    }
    return out;
}

std::vector<ExternalUtterance> fetch_thread_comments(const RedditCommentsOptions& opts) {
    const std::size_t max = opts.maxComments.value_or(500);
    const int depth = opts.depth.value_or(10);
    const std::string url = opts.endpoints.oauth_base + "/comments/" + url::encode_component(opts.submissionId) +
                            "?depth=" + std::to_string(depth) + "&limit=" + std::to_string(max) + "&raw_json=1";
    const Json data = get_json(url, opts.token, opts.userAgent, opts.fetchImpl);
    // data = [postListing, commentsListing]
    static const Json empty = Json::array();
    const Json* children = &empty;
    if (data.is_array() && data.size() > 1 && data[1].is_object()) {
        const Json& listing = json_object(data[1], "data");
        children = &json_array(listing, "children");
    }
    return flatten_comments(*children, RedditContext{opts.submissionId, opts.gameSlug, opts.permalink}, max);
}

std::vector<ExternalUtterance> fetch_reddit_discussions(const RedditFetchOptions& opts) {
    const std::string token = get_reddit_token(opts, opts.fetchImpl, opts.endpoints);
    const int max_threads = opts.maxThreads.value_or(5);
    const long long delay = opts.politeDelayMs.value_or(1500);
    RedditSearchOptions search;
    search.token = token;
    search.userAgent = opts.userAgent;
    search.query = opts.query;
    search.subreddit = opts.subreddit;
    search.limit = max_threads;
    search.fetchImpl = opts.fetchImpl;
    search.endpoints = opts.endpoints;
    auto threads = search_threads(search);
    if (max_threads >= 0 && threads.size() > static_cast<std::size_t>(max_threads)) threads.resize(static_cast<std::size_t>(max_threads));

    std::vector<ExternalUtterance> out;
    for (std::size_t i = 0; i < threads.size(); ++i) {
        const auto& t = threads[i];
        RedditCommentsOptions c;
        c.token = token;
        c.userAgent = opts.userAgent;
        c.submissionId = t.id;
        c.gameSlug = opts.gameSlug;
        c.permalink = t.permalink;
        c.maxComments = opts.maxCommentsPerThread.value_or(300);
        c.fetchImpl = opts.fetchImpl;
        c.endpoints = opts.endpoints;
        auto comments = fetch_thread_comments(c);
        const std::size_t n = comments.size();
        for (auto& u : comments) out.push_back(std::move(u));
        if (opts.onThread) opts.onThread(t.title, n, out.size());
        if (i + 1 < threads.size()) polite_sleep(delay);
    }
    return out;
}

} // namespace di::crawler::sources
