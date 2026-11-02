#pragma once
// 情報ゲート — 議論開始前の情報密度評価フェーズ (src/flow/information-gate.ts の移植、docs/information-gate.md)。
//
// 議論/改善フロー開始の直前に:
//   1. テーマの既存材料 (収集済みの外部の声) を集め、
//   2. LLM で情報密度 (sparse/moderate/rich) と不足観点 (gaps) を評価し、
//   3. 不足していれば不足観点を狙ったクエリでクロール → KG 取込し、
//   4. 再評価する (十分 or 反復上限まで)。
// 自動モード固定。LLM 障害時は議論を止めない (件数ベースの粗い fallback で degrade)。
// LLM の応答 JSON は上限つきで読む (extract_json_object の 1MiB・要素 5 万・深さ 32 + 本ファイルの件数上限)。
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "base/json.hpp"
#include "core/index.hpp"
#include "flow/discussion_paper.hpp"
#include "flow/learning_autocrawl.hpp"
#include "flow/tags.hpp"
#include "flow/voice_cache.hpp"
#include "persona_engine/llm/client.hpp"

namespace di::flow {

/** 情報密度の段階 ("sparse" < "moderate" < "rich")。 */
using InformationDensity = std::string;

bool is_density(const Json& v);
bool is_density(const std::string& s);
/** sparse=0 / moderate=1 / rich=2 (不明は 0)。 */
int density_rank(const InformationDensity& d);

/** 評価時に密度判定に使う外部の声の取得上限。 */
inline constexpr int ASSESS_LIMIT = 50;
/** LLM 応答から読む covered / gaps の件数上限 (移植で追加)。 */
inline constexpr std::size_t kMaxCoveredItems = 100;
inline constexpr std::size_t kMaxGapItems = 50;

struct InformationGap {
    /** 不足している観点。 */
    std::string aspect;
    /** なぜその観点が必要か。 */
    std::string reason;
    /** 不足を埋めるためのクロール検索クエリ。 */
    std::string query;
    /** LLM が推奨するソース (advisory; 実クロールは config 既定ソースを使う)。 */
    std::optional<AutoCrawlSource> source;
};
Json to_json(const InformationGap& g);

struct InformationEvaluation {
    InformationDensity density = "sparse";
    std::vector<std::string> covered;
    std::vector<InformationGap> gaps;
    /** LLM が提案した閾値ルールの Condition 候補 (raw)。検証は density_blackbox 側。 */
    std::optional<Json> proposedRule;
};
Json to_json(const InformationEvaluation& e);

/** 件数だけから粗く密度を見積もる (8 件以上 rich / 3 件以上 moderate / それ未満 sparse)。 */
InformationDensity fallback_density(std::size_t voice_count);

/** 評価プロンプトの材料テキスト (テスト用に公開)。 */
std::string build_material_text(const std::string& theme, const std::vector<FlowTag>& tags, const std::vector<ContextVoice>& voices);
struct EvalPrompt {
    std::string system;
    std::string prompt;
};
EvalPrompt build_eval_prompt(const std::string& material);

/** gaps の正規化 (aspect 空は捨てる・query 欠落は「テーマ 観点」・不正な source は捨てる)。 */
std::vector<InformationGap> normalize_gaps(const Json& raw, const std::string& theme);

struct EvaluateArgs {
    std::string theme;
    std::vector<FlowTag> tags;
    std::vector<ContextVoice> voices;
    persona_engine::LLMClient* llm = nullptr;
    std::optional<std::string> model;
    std::function<void(const std::string&)> warn;
};

/**
 * 収集済み材料から情報密度と不足観点を 1 回評価する (LLM 1 コール)。
 * LLM 失敗 / JSON 解析失敗は件数ベースの fallback に倒す (議論を止めない)。
 */
InformationEvaluation evaluate_information_density(const EvaluateArgs& args);
using EvaluateFn = std::function<InformationEvaluation(const EvaluateArgs&)>;

/** ゲートの動作設定 (config.flow.informationGating)。 */
struct InformationGateConfig {
    bool enabled = true;
    InformationDensity minDensity = "moderate";
    int maxLearnIterations = 2;
    int maxGapsPerIteration = 2;
    std::optional<std::string> model;
};

struct InformationGateDeps {
    core::Core* core = nullptr;
    std::string theme;
    std::vector<FlowTag> tags;
    std::string workspaceId;
    /** 評価用 LLM (呼び出し側でコストログ済みを渡す)。 */
    persona_engine::LLMClient* llm = nullptr;
    /** 議論と同じ外部の声検索 (= 密度評価の材料)。未指定なら材料 0 件扱い。 */
    ListExternalVoicesFn listExternalVoices;
    /** クロールに使う既定ソース (後方互換の単数指定)。 */
    std::optional<AutoCrawlSource> crawlSource;
    /** クロールに使うソース群 (複数ソース横断学習)。空なら [crawlSource]。 */
    std::vector<AutoCrawlSource> crawlSources;
    std::optional<int> maxItems;
    std::optional<std::string> youtubeApiKey;
    InformationGateConfig config;
    /** 取得→取込の差し替え (テスト用)。既定 collect_and_import。 */
    CollectAndImportFn collectAndImportFn;
    /** 密度評価の差し替え (density_blackbox でラップした評価器など)。既定 evaluate_information_density。 */
    EvaluateFn evaluateFn;
    std::function<void(const std::string&)> log;
    std::function<void(const std::string&)> warn;
};

struct InformationGateResult {
    bool ran = false;
    bool sufficient = false;
    InformationDensity initialDensity;
    InformationDensity finalDensity;
    int crawls = 0;
    int importedTotal = 0;
    std::vector<std::string> crawledQueries;
    std::string message;
};
Json to_json(const InformationGateResult& r);

/**
 * 情報ゲートを実行する。不足なら不足観点を狙って学習 → 再評価し、閾値に届くか反復上限まで回す。
 * 届かなくても議論は開始できるよう sufficient=false で返す。
 */
InformationGateResult run_information_gate(const InformationGateDeps& deps);

} // namespace di::flow
