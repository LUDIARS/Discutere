#pragma once
// 学習フロー (learning.md) — src/flow/learning.ts の移植。
//
// ゲームタイトル + テーマに即したユーザ意見 / メカニクスを収集・記録する。
// **LLM による議論はしない** (ペルソナ発話・ラウンド・投票なし)。議論フローの前段として実行する。
//   - 意見    → KG 外部発話 (import_external_utterances, source="feedback", 匿名)。極性は感情カスケード。
//   - メカニクス → data/games/<slug>.md frontmatter + Discatier Core mechanics (記録)。
// 個人データは保存しない (authorId は匿名固定、authorName は持たない)。
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "base/json.hpp"
#include "core/index.hpp"
#include "crawler/sentiment/cascade.hpp"
#include "flow/games_md.hpp"
#include "flow/learning_autocrawl.hpp"

namespace di::flow {

/** 学習フローが受ける 1 意見 (収集元は問わない。匿名)。 */
struct LearningOpinion {
    /** 意見本文 (個人名・アカウント名を含めない) */
    std::string content;
    /** 投稿時刻 epoch ms (省略時は記録時刻) */
    std::optional<double> postedAt;
};

/** ソース別の追加パラメータ (steam appId / website urls / subreddit)。 */
struct LearningSourceParams {
    std::optional<long long> appId;
    std::optional<std::vector<std::string>> urls;
    std::optional<std::string> subreddit;
};

/** 自動収集モードの指定 (① 類似ゲームの自動収集)。 */
struct LearningCrawlSpec {
    /** 収集ソース群。空なら収集しない。 */
    std::vector<AutoCrawlSource> sources;
    /** 検索クエリ (省略時はテーマ)。 */
    std::optional<std::string> query;
    /** 1 ソースあたり取込上限 (既定 200)。 */
    std::optional<int> maxItems;
    std::optional<std::string> youtubeApiKey;
    std::map<AutoCrawlSource, LearningSourceParams> specBySource;
};

struct LearningFlowDeps {
    core::Core* core = nullptr;
    std::vector<LearningOpinion> opinions;
    std::vector<GameMechanicEntry> mechanics;
    std::optional<LearningCrawlSpec> crawl;
    /** 感情カスケードの LLM (省略時は Tier 0 辞書のみ)。 */
    crawler::sentiment::CascadeClients sentimentClients;
    /** ワークスペース ID (既定 knowledge)。 */
    std::optional<std::string> workspaceId;
    /** slug 明示指定。省略時は derive_slug(gameTitle)。 */
    std::optional<std::string> slug;
    /** data/games ディレクトリ (既定 ./data/games)。 */
    std::optional<std::string> gamesDir;
    /** 自動収集の取得→取込関数 (テスト用に差し替え)。既定 collect_and_import。 */
    CollectAndImportFn collectAndImportFn;
    LogFn log;
    LogFn warn;
};

struct LearningFlowResult {
    std::string gameSlug;
    int opinionsRecorded = 0;
    int mechanicsRecorded = 0;
    /** 付与された極性の内訳 (positive/negative/neutral 件数。キー順を保つ)。 */
    Json polarityBreakdown = Json::object();
    int crawledImported = 0;
    /** ソース別のクロール取込内訳 (crawl 未指定なら空)。 */
    Json crawledBySource = Json::object();
};
Json to_json(const LearningFlowResult& r);

/** 学習フローを実行する。議論は一切しない (収集・記録のみ)。 */
LearningFlowResult run_learning_flow(const std::string& game_title, const std::string& theme, const LearningFlowDeps& deps);

} // namespace di::flow
