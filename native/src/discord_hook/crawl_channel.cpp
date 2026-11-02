#include "discord_hook/crawl_channel.hpp"

#include <algorithm>

#include "base/strings.hpp"
#include "base/url.hpp"
#include "crawler/sources/importer.hpp"
#include "crawler/sources/steam.hpp"
#include "crawler/sources/website.hpp"
#include "crawler/sources/youtube_comments.hpp"
#include "crawler/sources/youtube_quota.hpp"
#include "ludus/economy_analyzer.hpp"

namespace di::discord_hook {

namespace cs = crawler::sources;

Json to_json(const CrawlTarget& t) {
    Json j{{"kind", t.kind}, {"url", t.url}};
    if (t.appId) j["appId"] = *t.appId;
    if (t.videoId) j["videoId"] = *t.videoId;
    if (t.submissionId) j["submissionId"] = *t.submissionId;
    if (t.permalink) j["permalink"] = *t.permalink;
    return j;
}

Json to_json(const CrawlIngestResult& r) {
    Json j{{"url", r.url}, {"kind", r.kind}, {"gameSlug", r.gameSlug}, {"fetched", r.fetched}, {"imported", r.imported}, {"skipped", r.skipped}};
    if (r.error) j["error"] = *r.error;
    return j;
}

namespace {

bool is_ascii_alnum(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }

/** text[i..] が /https?:\/\//i に一致すればその長さ、しなければ 0。 */
std::size_t url_scheme_len(std::string_view text, std::size_t i) {
    auto ieq = [&](std::size_t pos, std::string_view lit) {
        return pos + lit.size() <= text.size() && str::iequals_ascii(text.substr(pos, lit.size()), lit);
    };
    if (!ieq(i, "http")) return 0;
    std::size_t p = i + 4;
    if (p < text.size() && (text[p] == 's' || text[p] == 'S')) {
        if (text.compare(p + 1, 3, "://") == 0) return 8;
    }
    if (text.compare(p, 3, "://") == 0) return 7;
    return 0;
}

/** URL 本体に含めない文字 ([\s<>"'）)】」])。 */
bool is_url_stop(std::uint32_t cp) {
    return str::is_js_space(cp) || cp == '<' || cp == '>' || cp == '"' || cp == '\'' || cp == ')' || cp == 0xFF09 || cp == 0x3011 || cp == 0x300D;
}

/** 末尾の [.,。、)）】」]+ を落とす。 */
std::string strip_trailing_punct(const std::string& u) {
    const auto cps = str::codepoints(u);
    std::size_t end = cps.size();
    while (end > 0) {
        const std::uint32_t c = cps[end - 1];
        if (c == '.' || c == ',' || c == 0x3002 || c == 0x3001 || c == ')' || c == 0xFF09 || c == 0x3011 || c == 0x300D) --end;
        else break;
    }
    return str::from_codepoints(std::vector<std::uint32_t>(cps.begin(), cps.begin() + static_cast<std::ptrdiff_t>(end)));
}

/** /\/app\/(\d+)/ の最初の一致の数字列。 */
std::optional<std::string> find_app_digits(const std::string& path) {
    std::size_t pos = 0;
    while ((pos = path.find("/app/", pos)) != std::string::npos) {
        std::size_t p = pos + 5;
        std::size_t s = p;
        while (p < path.size() && path[p] >= '0' && path[p] <= '9') ++p;
        if (p > s) return path.substr(s, p - s);
        pos += 1;
    }
    return std::nullopt;
}

bool is_word_or_dash(char c) { return is_ascii_alnum(c) || c == '_' || c == '-'; }

/** /\/(?:shorts|live|embed)\/([\w-]{6,})/ の最初の一致。 */
std::optional<std::string> find_youtube_path_id(const std::string& path) {
    for (std::size_t i = 0; i < path.size(); ++i) {
        if (path[i] != '/') continue;
        for (std::string_view kw : {"shorts/", "live/", "embed/"}) {
            if (path.compare(i + 1, kw.size(), kw) != 0) continue;
            std::size_t p = i + 1 + kw.size();
            const std::size_t s = p;
            while (p < path.size() && is_word_or_dash(path[p])) ++p;
            if (p - s >= 6) return path.substr(s, p - s);
        }
    }
    return std::nullopt;
}

/** /\/comments\/([a-z0-9]+)/i の最初の一致。 */
std::optional<std::string> find_reddit_id(const std::string& path) {
    for (std::size_t i = 0; i < path.size(); ++i) {
        if (path[i] != '/' || i + 10 > path.size() || !str::iequals_ascii(std::string_view(path).substr(i + 1, 9), "comments/")) continue;
        std::size_t p = i + 10;
        const std::size_t s = p;
        while (p < path.size() && is_ascii_alnum(path[p])) ++p;
        if (p > s) return path.substr(s, p - s);
    }
    return std::nullopt;
}

std::string strip_www(const std::string& host) { return str::starts_with(host, "www.") ? host.substr(4) : host; }

/** NFKD → 小文字 → [^a-z0-9]+ を "-" → 前後の "-" を除く → 48 文字 → 空なら "web" (ホスト名は ASCII)。 */
std::string slugify(const std::string& value) {
    const std::string s = str::js_slice(ludus::to_slug(value), 0, 48);
    return s.empty() ? std::string("web") : s;
}

} // namespace

std::vector<std::string> extract_urls(const std::string& text) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < text.size()) {
        const std::size_t scheme = url_scheme_len(text, i);
        if (scheme > 0) {
            std::size_t p = i + scheme;
            while (p < text.size()) {
                std::size_t q = p;
                const std::uint32_t cp = str::next_codepoint(text, q);
                if (is_url_stop(cp)) break;
                p = q;
            }
            if (p > i + scheme) {
                std::string u = strip_trailing_punct(text.substr(i, p - i));
                if (!u.empty()) out.push_back(std::move(u));
                i = p;
                continue;
            }
        }
        str::next_codepoint(text, i);
    }
    return out;
}

ParsedCrawlMessage parse_crawl_message(const std::string& content) {
    const std::string trimmed = str::trim(content);
    ParsedCrawlMessage out;
    std::string rest = trimmed;
    auto read_slug = [&](std::size_t start, std::size_t& end) -> bool {
        if (start >= trimmed.size() || !is_ascii_alnum(trimmed[start])) return false;
        std::size_t p = start + 1;
        while (p < trimmed.size() && p - start - 1 < 63 && (is_ascii_alnum(trimmed[p]) || trimmed[p] == '-')) ++p;
        end = p;
        return true;
    };
    auto skip_spaces = [&](std::size_t p) {
        while (p < trimmed.size()) {
            std::size_t q = p;
            if (!str::is_js_space(str::next_codepoint(trimmed, q))) break;
            p = q;
        }
        return p;
    };
    std::size_t end = 0;
    bool done = false;
    // /^\[([a-z0-9][a-z0-9-]{0,63})\]\s*/i
    if (!trimmed.empty() && trimmed[0] == '[' && read_slug(1, end) && end < trimmed.size() && trimmed[end] == ']') {
        out.gameSlug = str::to_lower_ascii(trimmed.substr(1, end - 1));
        rest = trimmed.substr(skip_spaces(end + 1));
        done = true;
    }
    // /^([a-z0-9][a-z0-9-]{0,63}):\s+(?=https?:\/\/)/i
    if (!done && read_slug(0, end) && end < trimmed.size() && trimmed[end] == ':') {
        const std::size_t after = skip_spaces(end + 1);
        if (after > end + 1 && url_scheme_len(trimmed, after) > 0) {
            out.gameSlug = str::to_lower_ascii(trimmed.substr(0, end));
            rest = trimmed.substr(after);
        }
    }
    out.urls = extract_urls(rest);
    return out;
}

CrawlTarget classify_crawl_url(const std::string& raw_url) {
    CrawlTarget website{"website", raw_url, std::nullopt, std::nullopt, std::nullopt, std::nullopt};
    const auto u = url::parse(raw_url);
    if (!u || (u->scheme != "http" && u->scheme != "https")) return website;
    const std::string host = strip_www(u->host);

    // Steam: store.steampowered.com/app/<id>/... or steamcommunity.com/app/<id>
    if (host == "store.steampowered.com" || host == "steamcommunity.com") {
        if (const auto digits = find_app_digits(u->path)) {
            // Number(m[1])。18 桁を超える (安全に整数で持てない) appId はそのゲームが無いので website 扱い (移植で追加)。
            if (digits->size() <= 18) {
                if (const auto id = str::parse_int(*digits)) {
                    CrawlTarget t = website;
                    t.kind = "steam";
                    t.appId = *id;
                    return t;
                }
            }
        }
    }

    // YouTube watch: youtube.com/watch?v=<id> / youtu.be/<id> / youtube.com/shorts/<id>
    if (host == "youtube.com" || host == "m.youtube.com" || host == "youtu.be") {
        std::optional<std::string> video_id;
        if (host == "youtu.be") {
            const std::string p = u->path.size() > 1 ? u->path.substr(1) : std::string();
            if (!p.empty()) video_id = p;
        } else if (u->path == "/watch") {
            for (const auto& [k, v] : url::parse_query(u->query))
                if (k == "v") {
                    if (!v.empty()) video_id = v;
                    break;
                }
        } else {
            video_id = find_youtube_path_id(u->path);
        }
        if (video_id) {
            CrawlTarget t = website;
            t.kind = "youtube";
            t.videoId = video_id;
            return t;
        }
    }

    // Reddit: reddit.com/r/<sub>/comments/<id>/... or /comments/<id>
    if (host == "reddit.com" || host == "old.reddit.com" || host == "np.reddit.com") {
        if (const auto id = find_reddit_id(u->path)) {
            CrawlTarget t = website;
            t.kind = "reddit";
            t.submissionId = id;
            t.permalink = u->path;
            return t;
        }
    }
    return website;
}

std::string slug_from_target(const CrawlTarget& t) {
    if (t.kind == "steam" && t.appId && *t.appId != 0) return "steam-" + std::to_string(*t.appId);
    if (t.kind == "youtube" && t.videoId && !t.videoId->empty()) return "yt-" + *t.videoId;
    if (t.kind == "reddit" && t.submissionId && !t.submissionId->empty()) return "reddit-" + *t.submissionId;
    const auto u = url::parse(t.url);
    if (!u) return "web";
    return slugify(strip_www(u->host));
}

namespace {

std::vector<cs::ExternalUtterance> fetch_for_target(const CrawlTarget& t, const std::string& game_slug, const CrawlDeps& deps) {
    if (t.kind == "steam" && t.appId && *t.appId != 0) {
        cs::SteamFetchOptions o;
        o.appId = *t.appId;
        o.gameSlug = game_slug;
        o.maxReviews = CrawlCaps::steamReviews;
        o.fetchImpl = deps.fetchImpl;
        return cs::fetch_steam_reviews(o);
    }
    if (t.kind == "youtube" && t.videoId && !t.videoId->empty()) {
        std::optional<std::string> key = deps.getYoutubeApiKey ? deps.getYoutubeApiKey() : deps.youtubeApiKey;
        if (!key || key->empty()) throw std::runtime_error("YouTube API key 未設定 (DISCUTERE_YOUTUBE_API_KEY)");
        cs::YoutubeCommentsOptions o;
        o.videoId = *t.videoId;
        o.gameSlug = game_slug;
        o.apiKey = *key;
        o.maxComments = CrawlCaps::youtubeComments;
        o.quota = cs::create_quota_tracker();
        o.fetchImpl = deps.fetchImpl;
        return cs::fetch_video_comments(o);
    }
    if (t.kind == "reddit" && t.submissionId && !t.submissionId->empty()) {
        if (!deps.reddit) throw std::runtime_error("Reddit 認証未設定 (DISCUTERE_REDDIT_CLIENT_ID/SECRET)");
        const std::string token = cs::get_reddit_token(*deps.reddit, deps.fetchImpl);
        cs::RedditCommentsOptions o;
        o.token = token;
        o.userAgent = deps.reddit->userAgent;
        o.submissionId = *t.submissionId;
        o.gameSlug = game_slug;
        o.permalink = t.permalink;
        o.maxComments = CrawlCaps::redditComments;
        o.fetchImpl = deps.fetchImpl;
        return cs::fetch_thread_comments(o);
    }
    cs::WebsiteFetchOptions o;
    o.urls = {t.url};
    o.gameSlug = game_slug;
    o.fetchImpl = deps.fetchImpl;
    return cs::fetch_website_articles(o);
}

} // namespace

std::vector<CrawlIngestResult> ingest_crawl_urls(const CrawlDeps& deps, const CrawlIngestInput& input) {
    std::vector<CrawlIngestResult> out;
    const std::size_t n = std::min(input.urls.size(), kCrawlMaxUrlsPerMessage);
    for (std::size_t idx = 0; idx < n; ++idx) {
        const std::string& u = input.urls[idx];
        const CrawlTarget target = classify_crawl_url(u);
        const std::string game_slug = input.gameSlugHint ? *input.gameSlugHint : slug_from_target(target);
        CrawlIngestResult base{u, target.kind, game_slug, 0, 0, 0, std::nullopt};
        auto core = deps.createCore();
        auto ingested = deps.openIngested ? deps.openIngested() : cs::open_ingested_store();
        auto attribution = deps.openAttribution ? deps.openAttribution() : cs::open_attribution_store();
        // website (長文) は要約器があれば要約/raw 2 層で取り込む (id=67)。
        const bool use_raw_store = target.kind == "website" && deps.summarizer;
        std::unique_ptr<cs::RawStore> raw_store;
        if (use_raw_store) raw_store = deps.openRaw ? deps.openRaw() : cs::open_raw_store();
        try {
            auto items = fetch_for_target(target, game_slug, deps);
            base.fetched = static_cast<int>(items.size());
            if (target.kind == "website" && deps.summarizer) cs::summarize_items(items, *deps.summarizer);
            cs::ExternalImportOptions opts;
            opts.workspaceId = deps.workspaceId;
            opts.ingested = ingested.get();
            opts.attribution = attribution.get();
            opts.rawStore = raw_store.get();
            const auto r = cs::import_external_utterances(*core, items, opts);
            base.imported = static_cast<int>(r.utterances);
            base.skipped = static_cast<int>(r.skipped);
        } catch (const std::exception& err) {
            base.error = err.what();
        }
        auto quiet = [](auto&& fn) {
            try {
                fn();
            } catch (...) {
            }
        };
        if (raw_store) quiet([&] { raw_store->close(); });
        quiet([&] { attribution->close(); });
        quiet([&] { ingested->close(); });
        quiet([&] { core->close(); });
        out.push_back(std::move(base));
    }
    return out;
}

std::string format_crawl_summary(const std::vector<CrawlIngestResult>& results) {
    std::vector<std::string> lines;
    int total = 0;
    for (const auto& r : results) {
        total += r.imported;
        if (r.error) {
            lines.push_back("⚠️ [" + r.kind + "] " + r.url + "\n   └ 失敗: " + *r.error);
            continue;
        }
        const std::string icon = r.imported > 0 ? "✅" : "➖";
        lines.push_back(icon + " [" + r.kind + "] " + r.gameSlug + " ← " + std::to_string(r.imported) + "件取込 (skip " + std::to_string(r.skipped) +
                        ") / " + r.url);
    }
    return "📥 **データ追加** (計 " + std::to_string(total) + " 件)\n" + str::join(lines, "\n");
}

} // namespace di::discord_hook
