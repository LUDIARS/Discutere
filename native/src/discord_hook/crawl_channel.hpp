#pragma once
// データクロール用チャンネル — 貼られたリンクから外部議論データを学習する (id=60)。
// (src/discord-hook/crawl-channel.ts の移植)
//
// 専用チャンネル (config `discord.crawlChannelIds`) に URL が貼られたら、URL の種類を
// 判定 (steam / youtube / reddit / website) して該当 collector を回し、Discatier Core に取り込む。
// 取り込み結果は「データ追加」通知チャンネル (system_channel) に投稿する (crawl_handler)。
// URL 判定・slug 導出・メッセージ解析は純関数 (テスト可能)、ingest_crawl_urls が I/O。
// website は利用者が貼った URL なので crawler/sources/website の SSRF 対策 (私設アドレス拒否) のまま取得する。
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/json.hpp"
#include "core/index.hpp"
#include "crawler/sources/attribution_store.hpp"
#include "crawler/sources/ingested_store.hpp"
#include "crawler/sources/raw_store.hpp"
#include "crawler/sources/reddit.hpp"
#include "crawler/sources/summarize.hpp"
#include "crawler/sources/types.hpp"

namespace di::discord_hook {

/** "steam" | "youtube" | "reddit" | "website" */
using CrawlKind = std::string;

struct CrawlTarget {
    CrawlKind kind;
    std::string url;
    /** steam appId (kind=steam) */
    std::optional<long long> appId;
    /** youtube videoId (kind=youtube) */
    std::optional<std::string> videoId;
    /** reddit submission id + permalink (kind=reddit) */
    std::optional<std::string> submissionId;
    std::optional<std::string> permalink;
    bool operator==(const CrawlTarget&) const = default;
};
Json to_json(const CrawlTarget& t);

/** 1 URL あたりの取得上限 (1 回の貼り付けで KG を埋め尽くさないための安全弁)。 */
struct CrawlCaps {
    static constexpr std::size_t steamReviews = 500;
    static constexpr std::size_t youtubeComments = 500;
    static constexpr std::size_t redditComments = 500;
    static constexpr std::size_t websiteArticles = 1;
};
/** 1 メッセージから取り込む URL 数の上限 (移植で追加。超えた分は取り込まない)。 */
inline constexpr std::size_t kCrawlMaxUrlsPerMessage = 20;
/** 投稿するまとめの上限 (Discord の本文上限 2000 文字。移植で追加)。 */
inline constexpr std::size_t kCrawlSummaryMaxChars = 2000;

/** メッセージ本文から URL を抽出 (/https?:\/\/[^\s<>"'）)】」]+/gi、末尾の句読点を落とす)。 */
std::vector<std::string> extract_urls(const std::string& text);

struct ParsedCrawlMessage {
    std::optional<std::string> gameSlug;
    std::vector<std::string> urls;
    bool operator==(const ParsedCrawlMessage&) const = default;
};
/** 先頭に `[slug]` / `slug: ` があればゲーム slug ヒントとして拾い、残りから URL を抽出する。 */
ParsedCrawlMessage parse_crawl_message(const std::string& content);

/** URL → 取得元種別 + パラメータ。 */
CrawlTarget classify_crawl_url(const std::string& raw_url);

/** slug ヒントが無いとき、URL 種別から fallback slug を導出。 */
std::string slug_from_target(const CrawlTarget& t);

struct CrawlIngestResult {
    std::string url;
    CrawlKind kind;
    std::string gameSlug;
    int fetched = 0;
    int imported = 0;
    int skipped = 0;
    std::optional<std::string> error;
};
Json to_json(const CrawlIngestResult& r);

struct CrawlDeps {
    std::function<std::unique_ptr<core::Core>()> createCore;
    std::string workspaceId;
    /** YouTube Data API key (未設定なら youtube URL はエラーにする) */
    std::optional<std::string> youtubeApiKey;
    std::function<std::optional<std::string>()> getYoutubeApiKey;
    /** Reddit OAuth 認証 (未設定なら reddit URL はエラーにする) */
    std::optional<crawler::sources::RedditCredentials> reddit;
    /** website 記事の要約器 (id=67)。渡せば長文 website を要約/raw 2 層で取り込む。 */
    std::shared_ptr<crawler::sources::Summarizer> summarizer;
    /** 取得の差し替え (テスト用。空なら各 collector の既定 = http::fetch)。 */
    crawler::sources::FetchFn fetchImpl;
    /** sidecar を開く (テスト用。空なら既定パス)。 */
    std::function<std::unique_ptr<crawler::sources::IngestedStore>()> openIngested;
    std::function<std::unique_ptr<crawler::sources::AttributionStore>()> openAttribution;
    std::function<std::unique_ptr<crawler::sources::RawStore>()> openRaw;
};

struct CrawlIngestInput {
    std::vector<std::string> urls;
    std::optional<std::string> gameSlugHint;
};

/** URL 群を取得 → Discatier Core に取り込み、結果サマリを返す (URL ごとに独立、失敗は記録)。 */
std::vector<CrawlIngestResult> ingest_crawl_urls(const CrawlDeps& deps, const CrawlIngestInput& input);

/** 結果サマリを 1 行/件の人間向けテキストに整形 (Discord 投稿用)。 */
std::string format_crawl_summary(const std::vector<CrawlIngestResult>& results);

} // namespace di::discord_hook
