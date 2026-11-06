#pragma once
// src/flow/web/flow-prep.ts の移植: 議論フロー WebUI の「開始前 情報準備」ヘルパ群 (routes から分離 / SRP)。
//
// - ペーパー編集ゲートの有効判定 (web_paper_gate_enabled)
// - リクエストボディ + config から自動クロール指定を組み立て (build_auto_crawl_spec / build_learning_crawl)
// - 議論/改善の開始前に情報を整える (情報ゲート → 旧 autoCrawl フォールバック)
//
// いずれも HTTP ハンドラ本体 (routes) から呼ばれる。FlowWebDeps はここを単一定義とする。
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "base/json.hpp"
#include "crawler/sentiment/cascade.hpp"
#include "flow/anatomia/client.hpp"
#include "flow/dispatch.hpp"
#include "flow/information_gate_runner.hpp"
#include "flow/learning.hpp"
#include "flow/learning_autocrawl.hpp"
#include "flow/notion_link.hpp"
#include "flow/spec_source.hpp"
#include "flow/tags.hpp"
#include "flow/voice_cache.hpp"
#include "persona_engine/llm/client.hpp"

namespace di::flow::web {

struct FlowWebDeps {
    std::string workspaceId;
    /** フロー実行用 LLM クライアント (必須)。 */
    persona_engine::LLMClientPtr llm;
    /** Discatier Core を開くファクトリ (learning / 情報ゲートに必要)。 */
    CoreOpenFn openCore;
    ListExternalVoicesFn listExternalVoices;
    crawler::sentiment::CascadeClients sentimentClients;
    std::optional<std::string> gamesDir;
    std::optional<std::string> youtubeApiKey;
    std::function<std::optional<std::string>()> getYoutubeApiKey;

    // ── C++ 版の追加: 外部 I/O の差し替え口 (テストで偽物を入れる。既定は本物) ──
    /** 旧 autoCrawl の collector / 取込。 */
    CollectorMap collectors;
    ImportItemsFn importItems;
    /** 情報ゲートの取得→取込 / 密度評価。 */
    CollectAndImportFn gateCollectAndImport;
    EvaluateFn gateEvaluate;
    /** 仕様書 URL / GitHub の取得 (fetch) とサイドカー抽出。 */
    std::function<http::ClientResponse(const http::ClientRequest&)> specFetch;
    SpecSidecarExtractor specExtractor;
    /** gh auth token (既定 get_github_cli_token)。 */
    std::function<std::optional<std::string>()> githubToken;
    /** Notion のクロール (既定 Canalis サイドカー)。 */
    NotionUrlCrawler notionCrawler;
    /** Anatomia CLI (既定 default_anatomia_cli_runner)。 */
    anatomia::AnatomiaCliRunner anatomiaRunner;
};

std::optional<std::string> resolve_youtube_api_key(const FlowWebDeps& d);

/** Web 議論/改善をペーパー編集ゲート経由にするか (enabled か webCanonical のどちらか)。 */
bool web_paper_gate_enabled();

/**
 * リクエストボディ (learningSource / learningQuery / learningAppId / learningUrls / learningSubreddit) +
 * config 既定から自動クロール指定を組み立てる。enabled=false / 不正ソースは nullopt。
 */
std::optional<AutoCrawlSpec> build_auto_crawl_spec(const Json& body);
Json to_json(const AutoCrawlSpec& s);

/**
 * 学習フローの自動収集指定を組み立てる (① 類似ゲームの自動収集)。
 *   - UI が learningSource を明示した場合は、その単一ソース (steam/website は appId/urls 付き) を使う。
 *   - 未指定なら config.flow.autoCrawl の自動経路ソース横断 (niconico + キーがあれば youtube)。
 * 収集可能なソースが無ければ nullopt (= 収集しない)。
 */
std::optional<LearningCrawlSpec> build_learning_crawl(const Json& body, const std::optional<std::string>& youtube_api_key);
Json to_json(const LearningCrawlSpec& s);

/**
 * 議論/改善の開始前に情報を整える。
 *   1. 情報ゲート (LLM が情報密度を評価し、不足観点を狙って学習 → 再評価) が有効ならそれを実行。
 *   2. ゲート対象外 (無効/フロー違い/Core 無し) なら、従来のカウント閾値 autoCrawl にフォールバック。
 * いずれも失敗は議論を止めない (graceful)。
 */
void prepare_information_before_flow(const FlowKind& kind, const std::string& theme, const std::vector<FlowTag>& tags,
                                     const std::string& session_id, const std::optional<AutoCrawlSpec>& spec, const FlowWebDeps& d);

/**
 * 旧来の自動クロール (テーマの外部の声が minVoices 件未満なら指定ソースでクロール → 取込)。
 * core 未設定 / 自動クロール無効ならスキップ。クロール失敗は議論を止めない (graceful)。
 */
void legacy_auto_crawl_before_flow(const FlowKind& kind, const std::string& theme, const std::optional<AutoCrawlSpec>& spec,
                                   const FlowWebDeps& d);

} // namespace di::flow::web
