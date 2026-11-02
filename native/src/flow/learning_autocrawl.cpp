#include "flow/learning_autocrawl.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>

#include "base/env.hpp"
#include "base/strings.hpp"
#include "crawler/sources/attribution_store.hpp"
#include "crawler/sources/importer.hpp"
#include "crawler/sources/ingested_store.hpp"
#include "crawler/sources/niconico.hpp"
#include "crawler/sources/steam.hpp"
#include "crawler/sources/website.hpp"
#include "crawler/sources/youtube_comments.hpp"
#include "crawler/sources/youtube_videos.hpp"
#include "ludus/economy_analyzer.hpp"

namespace di::flow {

namespace cs = crawler::sources;

const std::vector<AutoCrawlSource>& auto_crawl_sources() {
    static const std::vector<AutoCrawlSource> v{"niconico", "youtube", "steam", "website", "reddit"};
    return v;
}

bool is_auto_crawl_source(const std::string& s) {
    const auto& v = auto_crawl_sources();
    return std::find(v.begin(), v.end(), s) != v.end();
}

std::optional<cs::RedditCredentials> reddit_credentials_from_env() {
    const auto id = env::get_nonempty("DISCUTERE_REDDIT_CLIENT_ID");
    const auto secret = env::get_nonempty("DISCUTERE_REDDIT_CLIENT_SECRET");
    if (!id || !secret) return std::nullopt;
    cs::RedditCredentials c;
    c.clientId = *id;
    c.clientSecret = *secret;
    c.userAgent = env::get("DISCUTERE_REDDIT_USER_AGENT").value_or("LUDIARS-Discutere/0.1 (external discussion crawler)");
    return c;
}

std::vector<AutoCrawlSource> resolve_auto_crawl_sources(const std::vector<std::string>& sources, const std::optional<std::string>& youtube_api_key) {
    const bool has_yt = youtube_api_key && !youtube_api_key->empty();
    auto auto_usable = [&](const std::string& s) {
        return s == "niconico" || (s == "youtube" && has_yt) || (s == "reddit" && reddit_credentials_from_env().has_value());
    };
    std::vector<std::string> merged = sources;
    if (has_yt) merged.push_back("youtube");
    std::vector<AutoCrawlSource> out;
    std::set<std::string> seen;
    for (const auto& s : merged) {
        if (!seen.insert(s).second) continue;
        if (is_auto_crawl_source(s) && auto_usable(s)) out.push_back(s);
    }
    return out;
}

std::string derive_slug(const std::string& theme) {
    std::string slug = ludus::to_slug(theme);
    if (!slug.empty()) return slug;
    // theme.trim().replace(/\s+/g, "-").slice(0, 48)
    const std::string t = str::trim(theme);
    std::string dashed;
    bool in_space = false;
    std::size_t i = 0;
    while (i < t.size()) {
        const std::size_t start = i;
        const std::uint32_t cp = str::next_codepoint(t, i);
        if (str::is_js_space(cp)) {
            if (!in_space) dashed += '-';
            in_space = true;
        } else {
            in_space = false;
            dashed.append(t, start, i - start);
        }
    }
    slug = str::js_slice(dashed, 0, 48);
    return slug.empty() ? std::string("discussion") : slug;
}

std::string resolve_query(const CollectorContext& ctx) {
    const std::string q = ctx.spec.query ? str::trim(*ctx.spec.query) : std::string();
    return q.empty() ? ctx.theme : q;
}

const CollectorMap& default_collectors() {
    static const CollectorMap m{
        {"niconico",
         [](const CollectorContext& ctx) {
             cs::NicoFetchOptions o;
             o.gameSlug = ctx.gameSlug;
             o.query = resolve_query(ctx);
             o.maxVideos = 5;
             o.maxComments = ctx.maxItems;
             return cs::fetch_niconico_discussions(o);
         }},
        {"steam",
         [](const CollectorContext& ctx) {
             if (!ctx.spec.appId || *ctx.spec.appId == 0) throw std::runtime_error("steam クロールには appId が必要です");
             cs::SteamFetchOptions o;
             o.appId = *ctx.spec.appId;
             o.gameSlug = ctx.gameSlug;
             o.maxReviews = static_cast<std::size_t>(std::max(0, ctx.maxItems));
             return cs::fetch_steam_reviews(o);
         }},
        {"website",
         [](const CollectorContext& ctx) {
             if (!ctx.spec.urls || ctx.spec.urls->empty()) throw std::runtime_error("website クロールには 1 つ以上の URL が必要です");
             // 利用者が指定する URL。website の既定 fetch は私設アドレスを拒否する (SSRF 対策)。
             cs::WebsiteFetchOptions o;
             o.urls = *ctx.spec.urls;
             o.gameSlug = ctx.gameSlug;
             return cs::fetch_website_articles(o);
         }},
        {"youtube",
         [](const CollectorContext& ctx) {
             if (!ctx.youtubeApiKey || ctx.youtubeApiKey->empty())
                 throw std::runtime_error("youtube クロールには DISCUTERE_YOUTUBE_API_KEY が必要です");
             cs::SearchDiscoverOptions so;
             so.apiKey = *ctx.youtubeApiKey;
             so.query = resolve_query(ctx);
             so.maxResults = 5;
             so.order = "relevance";
             const auto videos = cs::discover_videos_by_search(so);
             std::vector<cs::ExternalUtterance> out;
             const std::size_t max_items = static_cast<std::size_t>(std::max(0, ctx.maxItems));
             for (const auto& v : videos) {
                 if (out.size() >= max_items) break;
                 try {
                     cs::YoutubeCommentsOptions co;
                     co.videoId = v.videoId;
                     co.gameSlug = ctx.gameSlug;
                     co.apiKey = *ctx.youtubeApiKey;
                     co.maxComments = max_items - out.size();
                     auto comments = cs::fetch_video_comments(co);
                     for (auto& c : comments) out.push_back(std::move(c));
                 } catch (...) {
                     // 1 動画の失敗は全体を止めない
                 }
             }
             if (out.size() > max_items) out.resize(max_items);
             return out;
         }},
        {"reddit",
         [](const CollectorContext& ctx) {
             const auto creds = reddit_credentials_from_env();
             if (!creds) throw std::runtime_error("reddit crawler requires DISCUTERE_REDDIT_CLIENT_ID/SECRET");
             const int max_threads = 4;
             cs::RedditFetchOptions o;
             static_cast<cs::RedditCredentials&>(o) = *creds;
             o.gameSlug = ctx.gameSlug;
             o.query = resolve_query(ctx);
             o.subreddit = ctx.spec.subreddit;
             o.maxThreads = max_threads;
             o.maxCommentsPerThread = static_cast<std::size_t>(std::max(1, (ctx.maxItems + max_threads - 1) / max_threads));
             auto items = cs::fetch_reddit_discussions(o);
             const std::size_t max_items = static_cast<std::size_t>(std::max(0, ctx.maxItems));
             if (items.size() > max_items) items.resize(max_items);
             return items;
         }},
    };
    return m;
}

int count_learning_voices(const std::string& theme, const ListExternalVoicesFn& list_external_voices, int limit) {
    if (!list_external_voices) return 0;
    try {
        return static_cast<int>(list_external_voices({theme}, limit).size());
    } catch (...) {
        return 0;
    }
}

int default_import(core::Core* core, const std::vector<cs::ExternalUtterance>& items, const std::string& workspace_id) {
    if (!core) throw std::invalid_argument("default_import requires a core");
    auto ingested = cs::open_ingested_store();
    auto attribution = cs::open_attribution_store();
    struct Closer {
        cs::IngestedStore& i;
        cs::AttributionStore& a;
        ~Closer() {
            try {
                a.close();
            } catch (...) {
            }
            try {
                i.close();
            } catch (...) {
            }
        }
    } closer{*ingested, *attribution};
    cs::ExternalImportOptions opts;
    opts.workspaceId = workspace_id;
    opts.ingested = ingested.get();
    opts.attribution = attribution.get();
    return static_cast<int>(cs::import_external_utterances(*core, items, opts).utterances);
}

namespace {

const Collector* find_collector(const CollectorMap& overrides, const std::string& source) {
    if (auto it = overrides.find(source); it != overrides.end() && it->second) return &it->second;
    const auto& d = default_collectors();
    if (auto it = d.find(source); it != d.end()) return &it->second;
    return nullptr;
}

void call(const LogFn& fn, const std::string& m) {
    if (fn) fn(m);
}

} // namespace

CollectAndImportResult collect_and_import(const CollectAndImportDeps& deps) {
    const int max_items = deps.maxItems.value_or(200);
    const ImportItemsFn import_items = deps.importItems ? deps.importItems : ImportItemsFn(default_import);
    const Collector* collector = find_collector(deps.collectors, deps.spec.source);
    if (!collector) {
        call(deps.warn, "未対応のクロールソース: " + deps.spec.source);
        return {};
    }
    const auto items = (*collector)(CollectorContext{deps.slug, deps.theme, deps.spec, max_items, deps.youtubeApiKey});
    if (items.empty()) return {};
    const int imported = import_items(deps.core, items, deps.workspaceId);
    call(deps.log, deps.spec.source + ": " + std::to_string(items.size()) + " 件取得 → " + std::to_string(imported) + " 件を KG に取込");
    return CollectAndImportResult{static_cast<int>(items.size()), imported};
}

Json to_json(const EnsureLearningDataResult& r) {
    return Json{{"crawled", r.crawled}, {"voicesBefore", r.voicesBefore}, {"imported", r.imported}, {"skipped", r.skipped}, {"message", r.message}};
}

EnsureLearningDataResult ensure_learning_data(const EnsureLearningDataDeps& deps) {
    const int max_items = deps.maxItems.value_or(200);
    EnsureLearningDataResult r;
    r.voicesBefore = count_learning_voices(deps.theme, deps.listExternalVoices, std::max(deps.minVoices, 1));
    if (r.voicesBefore >= deps.minVoices) {
        r.skipped = true;
        r.message = "学習データ充分 (外部の声 " + std::to_string(r.voicesBefore) + " 件 ≥ " + std::to_string(deps.minVoices) + ")";
        return r;
    }
    if (!find_collector(deps.collectors, deps.spec.source)) {
        call(deps.warn, "未対応のクロールソース: " + deps.spec.source);
        r.message = "未対応のソース: " + deps.spec.source;
        return r;
    }
    call(deps.log, "学習データ不足 (外部の声 " + std::to_string(r.voicesBefore) + " 件 < " + std::to_string(deps.minVoices) + ") → " +
                       deps.spec.source + " でクロール開始");
    CollectAndImportDeps c;
    c.core = deps.core;
    c.theme = deps.theme;
    c.slug = deps.slug;
    c.workspaceId = deps.workspaceId;
    c.spec = deps.spec;
    c.maxItems = max_items;
    c.youtubeApiKey = deps.youtubeApiKey;
    c.collectors = deps.collectors;
    c.importItems = deps.importItems;
    c.log = deps.log;
    c.warn = deps.warn;
    const auto got = collect_and_import(c);
    if (got.collected == 0) {
        r.message = deps.spec.source + ": 取得 0 件 (議論はそのまま開始)";
        return r;
    }
    r.crawled = got.imported > 0;
    r.imported = got.imported;
    r.message = deps.spec.source + " で " + std::to_string(got.collected) + " 件取得 → " + std::to_string(got.imported) + " 件を学習データに追加";
    return r;
}

} // namespace di::flow
