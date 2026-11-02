#pragma once
// 議論開始時の自動学習クロール (事前学習の UI 化) — src/flow/learning-autocrawl.ts の移植。
//
// 議論/改善フロー開始の直前に呼ばれ:
//   1. テーマの学習データ (外部の声) が足りているか判定する (議論と同じ listExternalVoices)。
//   2. 不足していれば「指定された方法 (ソース)」でクロール → KG 取込してから議論を始める。
// ソースとパラメータは議論ごとに UI で指定でき、既定は config.flow.autoCrawl。
// collector は DI 境界 (default_collectors) にして、ネットワーク/API キー無しでテストできる。
// クロール失敗は議論を止めない (呼び出し側が catch して続行する想定 / graceful degrade)。
// website は利用者が指定する URL なので crawler/sources/website の SSRF 対策 (allow_private_network=false) のまま使う。
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/index.hpp"
#include "crawler/sources/reddit.hpp"
#include "crawler/sources/types.hpp"
#include "flow/voice_cache.hpp"

namespace di::flow {

/** 自動クロールがサポートするソース ("niconico" | "youtube" | "steam" | "website" | "reddit")。 */
using AutoCrawlSource = std::string;

const std::vector<AutoCrawlSource>& auto_crawl_sources();
bool is_auto_crawl_source(const std::string& s);

/** DISCUTERE_REDDIT_CLIENT_ID / SECRET があれば認証情報 (USER_AGENT は既定値あり)。 */
std::optional<crawler::sources::RedditCredentials> reddit_credentials_from_env();

/**
 * テーマだけから自動でクロールできるソースに絞る (情報ゲート / 学習フローの自動経路共通)。
 *   - niconico は常に可 (キー不要)。
 *   - youtube は API キーがあれば可。キーがあれば候補に自動追加する。
 *   - reddit は認証情報 (env) があれば可。
 *   - website / steam は URL / appId がテーマだけからは決まらないため自動経路では除外する。
 */
std::vector<AutoCrawlSource> resolve_auto_crawl_sources(const std::vector<std::string>& sources,
                                                        const std::optional<std::string>& youtube_api_key = std::nullopt);

/** テーマ → クロール発話に付ける gameSlug。日本語は to_slug が空になるためフォールバックする。 */
std::string derive_slug(const std::string& theme);

/** クロール方法の指定 (UI 上書き or config 既定)。 */
struct AutoCrawlSpec {
    AutoCrawlSource source;
    /** 検索クエリ (niconico/youtube)。省略時はテーマ文字列。 */
    std::optional<std::string> query;
    /** Steam appId (steam では必須)。 */
    std::optional<long long> appId;
    /** 取得対象 URL 群 (website では必須)。 */
    std::optional<std::vector<std::string>> urls;
    std::optional<std::string> subreddit;
};

struct CollectorContext {
    std::string gameSlug;
    std::string theme;
    AutoCrawlSpec spec;
    int maxItems = 200;
    std::optional<std::string> youtubeApiKey;
};

using Collector = std::function<std::vector<crawler::sources::ExternalUtterance>(const CollectorContext&)>;
using CollectorMap = std::map<AutoCrawlSource, Collector>;

/** 既定 collector 群 (本物のネットワーク取得)。テストでは差し替える。 */
const CollectorMap& default_collectors();

/** spec.query (trim) か、空ならテーマ。 */
std::string resolve_query(const CollectorContext& ctx);

using ImportItemsFn = std::function<int(core::Core* core, const std::vector<crawler::sources::ExternalUtterance>& items,
                                        const std::string& workspace_id)>;
using LogFn = std::function<void(const std::string&)>;

/** テーマに対する既存の外部の声件数 (議論と同じ検索)。例外は 0。 */
int count_learning_voices(const std::string& theme, const ListExternalVoicesFn& list_external_voices, int limit);

/** 外部の声を KG に取り込む既定実装 (dedup sidecar + 出所メタ付き)。ユーザーの声の収集でも使う。 */
int default_import(core::Core* core, const std::vector<crawler::sources::ExternalUtterance>& items, const std::string& workspace_id);

struct CollectAndImportDeps {
    core::Core* core = nullptr;
    std::string theme;
    /** クロール発話に付ける gameSlug。 */
    std::string slug;
    std::string workspaceId;
    AutoCrawlSpec spec;
    /** 1 回の取込上限。既定 200。 */
    std::optional<int> maxItems;
    std::optional<std::string> youtubeApiKey;
    /** collector 差し替え (テスト用)。 */
    CollectorMap collectors;
    /** 取込関数 差し替え (テスト用)。 */
    ImportItemsFn importItems;
    LogFn log;
    LogFn warn;
};

struct CollectAndImportResult {
    /** collector が取得した件数。 */
    int collected = 0;
    /** KG に取り込んだ件数。 */
    int imported = 0;
};

/**
 * 充足判定を挟まず、指定ソース/クエリで「取得 → 取込」だけを行う (情報ゲートの狙い撃ち学習で流用)。
 * collector の例外はそのまま投げる (呼び出し側が warn して続行)。
 */
CollectAndImportResult collect_and_import(const CollectAndImportDeps& deps);
using CollectAndImportFn = std::function<CollectAndImportResult(const CollectAndImportDeps&)>;

struct EnsureLearningDataDeps {
    core::Core* core = nullptr;
    std::string theme;
    std::string slug;
    std::string workspaceId;
    AutoCrawlSpec spec;
    /** これ未満の外部の声しか無ければクロールする。 */
    int minVoices = 0;
    std::optional<int> maxItems;
    /** 議論と同じ外部の声検索 (= 充足判定に流用)。 未指定なら常に不足扱い。 */
    ListExternalVoicesFn listExternalVoices;
    std::optional<std::string> youtubeApiKey;
    CollectorMap collectors;
    ImportItemsFn importItems;
    LogFn log;
    LogFn warn;
};

struct EnsureLearningDataResult {
    bool crawled = false;
    int voicesBefore = 0;
    int imported = 0;
    bool skipped = false;
    std::string message;
};
Json to_json(const EnsureLearningDataResult& r);

/** テーマの学習データが不足していれば指定ソースでクロール → 取込する。充分なら no-op (skipped=true)。 */
EnsureLearningDataResult ensure_learning_data(const EnsureLearningDataDeps& deps);

} // namespace di::flow
