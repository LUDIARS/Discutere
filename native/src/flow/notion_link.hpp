#pragma once
// src/flow/notion-link.ts の移植: ペーパー作成の入力に含まれる Notion リンクをクロールして本文 md に展開する。
//
// - 本文中の Notion URL の検出 (notion.so / notion.com / *.notion.site、末尾の句読点を除く) と「深さ:N」指示の読み取りは C++。
// - 取得そのもの (Notion API / Playwright による公開ページ) は @ludiars/canalis を Node のサイドカー
//   native/sidecar/canalis-crawl.ts で呼ぶ (token は Node 側で env NOTION_TOKEN から読む。C++ は token を持たない)。
// - 深さは既定 flow.notionLinks.maxDepth (2)。本文の「深さ:N」/「depth:N」指示で上書きできる (0..MAX_NOTION_DEPTH)。
// 取得失敗は warn して他リンク・議論を止めない (graceful)。
#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace di::flow {

/** 深さ指示の上限 (全階層取得の暴発を防ぐ)。 */
inline constexpr int MAX_NOTION_DEPTH = 5;
/** 1 リンクあたりのページ数の上限 (サイドカーの入力検査と同じ)。 */
inline constexpr int MAX_NOTION_PAGES = 200;
/** 1 本文から展開するリンク数の上限。 */
inline constexpr std::size_t MAX_NOTION_LINKS = 20;
/** サイドカーの出力上限 (1 リンクぶん)。 */
inline constexpr std::size_t kNotionSidecarMaxOutput = 32u << 20;

struct NotionLinkPage {
    std::string url;
    std::string title;
    int depth = 0;
    std::string markdown;
};

struct NotionCrawlResult {
    std::vector<NotionLinkPage> pages;
    /** 取得中の warn (移植前と同じ文言)。 */
    std::vector<std::string> warnings;
};

/** 1 リンクを取得する境界 (テストで差し替える)。失敗は例外 (what() が warn の理由になる)。 */
using NotionUrlCrawler = std::function<NotionCrawlResult(const std::string& url, int depth, int max_pages)>;

/** 既定: native/sidecar/canalis-crawl.ts を起動する。期限は ページ数 × 30 秒 + 60 秒 (上限 15 分)。 */
NotionCrawlResult run_canalis_crawl_sidecar(const std::string& url, int depth, int max_pages);
std::chrono::milliseconds notion_sidecar_timeout(int max_pages);

/** Notion のホスト (notion.so / notion.com / *.notion.site) を持つ http(s) URL か (Canalis の isNotionUrl)。 */
bool is_notion_url(std::string_view url);

/** 本文中の Notion URL を出現順・重複なしで列挙する (末尾の句読点は除く。Canalis の findNotionUrls)。 */
std::vector<std::string> find_notion_urls(std::string_view text);

/** 本文中の「深さ:N」指示を読む (0..MAX_NOTION_DEPTH にクランプ)。無ければ nullopt。 */
std::optional<int> parse_notion_depth(std::string_view text);

/** 取得したページ群を、深さに応じた見出し + 出典付きで連結する。 */
std::string render_notion_pages(const std::vector<NotionLinkPage>& pages, const std::string& fallback_url);

struct ExpandNotionOpts {
    /** 深さ (起点=0)。省略時は本文の深さ指示 → config 既定。 */
    std::optional<int> depth;
    /** 1 リンクあたりの最大ページ数。省略時は config 既定。 */
    std::optional<int> maxPages;
    NotionUrlCrawler crawler;
    std::function<void(const std::string&)> warn;
};

/** 1 リンクを md に解決する。 */
std::string resolve_notion_url(const std::string& url, int depth, int max_pages, const ExpandNotionOpts& opts = {});

/** 本文中の Notion リンクを全てクロールし、`# Notion 資料` 節の md にまとめて返す。リンクが無い / 全件失敗なら ""。 */
std::string expand_notion_links(const std::string& text, const ExpandNotionOpts& opts = {});

} // namespace di::flow
