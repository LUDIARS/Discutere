#pragma once
// Website (Web 記事) collector — spec/feature/crawler/EXTERNAL-SOURCES.md §4.5 (src/crawler/sources/website.ts の移植)。
//
// 任意の URL (レビュー / 考察ブログ / ニュース記事) を 1 件取得し、本文を抽出して
// 1 つの ExternalUtterance に正規化する。記事 = ひとりの論者の 1 意見として扱う。
//
// 利用者が指定した URL を取りに行くので allow_private_network=false のまま (SSRF 対策: 解決した IP を
// 検査して固定する。http::fetch が行う)。リダイレクト先も同じ検査を受ける。
// HTML からのテキスト抽出は di::str::strip_html (線形) を使う。HTML パーサーは持たない。
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "crawler/sources/types.hpp"

namespace di::crawler::sources {

/** 1 記事の本文上限 (UTF-16 単位。議論シードに十分かつ巨大ページの暴発を防ぐ)。 */
inline constexpr std::size_t MAX_CONTENT_CHARS = 8000;
/** 1 記事の HTML の上限 (移植で追加)。 */
inline constexpr std::size_t MAX_HTML_BYTES = 4u << 20;

struct ExtractedArticle {
    std::string title;
    /** 抽出した本文 (タグ除去・空白圧縮済み)。 */
    std::string text;
    /** 署名 (meta author 等)。取れなければ nullopt。 */
    std::optional<std::string> author;
};

/** URL を dedup / アンカー用に正規化 (hash 除去 + tracking パラメータ除去)。 */
std::string normalize_url(std::string_view raw_url);

/** 生 HTML → {title, text, author} を抽出 (純関数・線形)。 */
ExtractedArticle extract_article(std::string_view html);

struct WebsiteMapInput {
    std::string url;
    std::string gameSlug;
    double fetchedAt = 0;
    ExtractedArticle article;
};

/** 抽出済み記事 → ExternalUtterance (純関数)。1 記事 = 1 発話 (= 1 論者の意見)。 */
ExternalUtterance map_website_article(const WebsiteMapInput& input);

struct WebsiteFetchOptions {
    std::vector<std::string> urls;
    std::string gameSlug;
    /** 同一ドメインへの最小間隔 ms。既定 1500 */
    std::optional<long long> politeDelayMs;
    /** テスト用の差し替え口。既定 http::fetch (allow_private_network=false で呼ぶ)。 */
    FetchFn fetchImpl;
    /** 「取得時刻」の注入 (テスト決定性用)。既定 time::now_ms() */
    std::function<double()> now;
    std::function<void(std::size_t total)> onPage;
    /** 取得失敗時のハンドラ (既定: warn ログへ出して継続)。 */
    std::function<void(const std::string& url, const std::string& error)> onError;
};

/** URL 群を順に取得し、ExternalUtterance[] に正規化する (失敗 URL は skip)。 */
std::vector<ExternalUtterance> fetch_website_articles(const WebsiteFetchOptions& opts);

} // namespace di::crawler::sources
