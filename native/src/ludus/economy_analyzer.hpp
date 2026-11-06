#pragma once
// LLM によるメカニクス分解 (経済グラフ) — src/ludus/economy-analyzer.ts の移植。
//
// TS は ANTHROPIC_API_KEY で api.anthropic.com を直接呼び、LLM に 2 つの DB ツール
//   - query_guide_mechanics: ludus RDBMS (ludus_mechanics 表)
//   - search_discussion_knowledge: Core KG SQLite (utterances の LIKE 検索)
// を使わせていた。C++ では API キーで直接呼ぶ経路を持たず、LLM は LLMClient (claude -p) で呼ぶ。
// ツール呼び出しの代わりに、同じ 2 つの問い合わせを先に実行し (ゲーム名で 1 回ずつ + 攻略メカニクス上位の語で
// 議論知識を追加検索)、結果をプロンプトに載せる。出力 JSON・キャッシュ (ludus_economy_graphs) の形は同じ。
//
// メカニクスの粒度は意図的に粗い: engine / attrition / win_condition / resource / experience。
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/json.hpp"
#include "base/sqlite.hpp"
#include "core/index.hpp"
#include "persona_engine/llm/client.hpp"

namespace di::ludus {

/** GraphNode / GraphEdge は LLM の出力をそのまま保持する (TS も検証せずに保存していた。object 以外は捨てる)。 */
struct EconomyGraph {
    std::string gameTitle;
    Json nodes = Json::array();
    Json edges = Json::array();
    std::string summary;
    std::vector<std::string> economyLoops;
    std::string analyzedAt;
    double mechanicCount = 0;
};
/** キー順は TS のオブジェクトリテラル (gameTitle, nodes, edges, summary, economyLoops, analyzedAt, mechanicCount)。 */
Json to_json(const EconomyGraph& g);

inline constexpr const char* ANALYSIS_MODEL = "claude-haiku-4-5-20251001";
inline constexpr int ANALYSIS_MAX_TOKENS = 4096;
inline constexpr long long ANALYSIS_TIMEOUT_MS = 120'000;
/** LLM 応答の上限 (外部入力)。 */
inline constexpr std::size_t kEconomyMaxResponseBytes = 1u << 20;
/** キャッシュ (graph_json) を読む上限 (DB の値も外部入力)。 */
inline constexpr std::size_t kEconomyGraphJsonMaxBytes = 4u << 20;
/** 攻略メカニクスの語で議論知識を追加検索する件数 (移植で追加。ツールの代わり)。 */
inline constexpr int kEconomyExtraKeywordSearches = 5;

/** toSlug: 小文字化 → [^a-z0-9]+ を "-" → 前後の "-" を除く。 */
std::string to_slug(const std::string& game_title);

/** ```json … ``` があれば中身、無ければ最初の '{' 〜 最後の '}'、それも無ければ trim した全体 (TS の extractJson)。 */
std::string extract_json(const std::string& text);

/** プロンプト (ツールの説明をデータの説明に置き換えたもの)。 */
const std::string& economy_system_prompt();
/** ユーザーメッセージ (ゲーム名 + 事前に集めた DB の情報)。 */
std::string build_user_message(const std::string& game_title, const std::string& guide_text, const std::string& discussion_text);

/** query_guide_mechanics と同じ整形 (ludus_mechanics から最大 min(limit,50) 件)。 */
std::string query_guide_mechanics_text(sql::Database& main_db, const std::string& workspace_id, const std::string& game_title, int limit = 30);
/** search_discussion_knowledge と同じ整形 (KG の utterances を LIKE で最大 min(limit,20) 件)。KG が無ければ定型文。 */
std::string search_discussion_knowledge_text(core::Core* core, const std::string& workspace_id, const std::string& keyword, int limit = 10);

struct AnalyzeEconomyDeps {
    /** 分析に使う LLM (claude -p)。未設定なら分析しない (キャッシュは返す)。 */
    persona_engine::LLMClientPtr llm;
    /** ludus_mechanics / ludus_economy_graphs の接続 (nullptr = get_main_db())。 */
    sql::DatabasePtr mainDb;
    /** KG を開く (既定 active KG)。例外なら議論知識は「利用不可」の定型文。 */
    std::function<std::unique_ptr<core::Core>()> openCore;
};

struct AnalyzeEconomyResult {
    bool ok = false;
    EconomyGraph graph;
    bool cached = false;
    std::string error;
};

/** 分析する (同期。呼んだスレッドで LLM を待つ)。force でキャッシュを無視する。 */
AnalyzeEconomyResult analyze_economy(const std::string& workspace_id, const std::string& game_title, const AnalyzeEconomyDeps& deps,
                                     bool force = false);

/**
 * 背景で分析する (TS の `void analyzeEconomy(...)`)。同じ (workspace, gameTitle) の分析が走っていれば重ねない (移植で追加)。
 * @returns 受け付けたら true (キュー満杯 / 実行中は false)。
 */
bool analyze_economy_in_background(const std::string& workspace_id, const std::string& game_title, AnalyzeEconomyDeps deps,
                                   bool force = false);

} // namespace di::ludus
