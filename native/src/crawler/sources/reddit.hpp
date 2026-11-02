#pragma once
// Reddit collector (Phase 2) — spec/feature/crawler/EXTERNAL-SOURCES.md §4.3 (src/crawler/sources/reddit.ts の移植)。
//
// OAuth2 (script app, client_credentials grant) で app-only token を取り、
// subreddit / 全体検索でスレッドを漁り、各スレッドのコメントツリーを
// ExternalUtterance に正規化する。username は公開・安定な persona アンカー。
// 認証情報は呼び出し側が env から渡す (config に平文で置かない)。ログ・エラー文に出さない。
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "crawler/sources/types.hpp"

namespace di::crawler::sources {

/** 取得元 (テストで偽サーバに差し替える)。 */
struct RedditEndpoints {
    std::string oauth_base = "https://oauth.reddit.com";
    std::string token_url = "https://www.reddit.com/api/v1/access_token";
};

struct RedditCredentials {
    std::string clientId;
    std::string clientSecret;
    /** Reddit 必須の User-Agent */
    std::string userAgent;
};

struct RedditThreadRef {
    std::string id;
    std::string title;
    std::string subreddit;
    std::string permalink;
    double numComments = 0;
};

struct RedditCommentRaw {
    std::optional<std::string> name;  // "t1_xxxx"
    std::optional<std::string> id;
    std::optional<std::string> body;
    std::optional<std::string> author;
    std::optional<double> created_utc;  // epoch seconds
    std::optional<std::string> parent_id;
    std::optional<double> ups;
};
RedditCommentRaw reddit_comment_from_json(const Json& j);

struct RedditContext {
    std::string submissionId;
    std::string gameSlug;
    std::optional<std::string> permalink;
};

/** Reddit comment 1 件 → ExternalUtterance (純関数)。取り込み対象外なら nullopt。 */
std::optional<ExternalUtterance> map_reddit_comment(const RedditCommentRaw& raw, const RedditContext& ctx);

/** コメントツリー (listing children の JSON 配列) を先頭から順に平坦化 (max まで)。 */
std::vector<ExternalUtterance> flatten_comments(const Json& children, const RedditContext& ctx, std::size_t max);

/** client_credentials grant で app-only access token を取得。失敗は std::runtime_error (秘密値は含めない)。 */
std::string get_reddit_token(const RedditCredentials& creds, const FetchFn& fetch_impl = {}, const RedditEndpoints& endpoints = {});

struct RedditSearchOptions {
    std::string token;
    std::string userAgent;
    std::string query;
    std::optional<std::string> subreddit;
    /** relevance | top | new | comments。既定 "top" */
    std::optional<std::string> sort;
    /** hour | day | week | month | year | all。既定 "year" */
    std::optional<std::string> time;
    /** 取得スレッド上限 (1-100)。既定 25 */
    std::optional<int> limit;
    FetchFn fetchImpl;
    RedditEndpoints endpoints;
};

std::vector<RedditThreadRef> search_threads(const RedditSearchOptions& opts);

struct RedditCommentsOptions {
    std::string token;
    std::string userAgent;
    std::string submissionId;
    std::string gameSlug;
    std::optional<std::string> permalink;
    /** コメント取得上限。既定 500 */
    std::optional<std::size_t> maxComments;
    /** ツリー深さ。既定 10 */
    std::optional<int> depth;
    FetchFn fetchImpl;
    RedditEndpoints endpoints;
};

std::vector<ExternalUtterance> fetch_thread_comments(const RedditCommentsOptions& opts);

struct RedditFetchOptions : RedditCredentials {
    std::string gameSlug;
    std::string query;
    std::optional<std::string> subreddit;
    /** 漁るスレッド数の上限。既定 5 */
    std::optional<int> maxThreads;
    /** 1 スレッドあたりのコメント上限。既定 300 */
    std::optional<std::size_t> maxCommentsPerThread;
    /** スレッド間の最小間隔 ms。既定 1500 */
    std::optional<long long> politeDelayMs;
    FetchFn fetchImpl;
    RedditEndpoints endpoints;
    std::function<void(const std::string& title, std::size_t comments, std::size_t total)> onThread;
};

/** 検索 → 上位スレッド → コメント を一括取得する。 */
std::vector<ExternalUtterance> fetch_reddit_discussions(const RedditFetchOptions& opts);

} // namespace di::crawler::sources
