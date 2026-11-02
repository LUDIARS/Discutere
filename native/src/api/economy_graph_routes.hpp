#pragma once
// Ludus economy graph — REST API + loopback HTML graph viewer (src/api/economy-graph-routes.ts の移植)。
//
// Routes ("/" に mount する前提):
//   POST /api/ludus/analyze-economy        分析を背景で開始 (fire-and-forget)
//   GET  /api/ludus/economy-graph/:slug    graph JSON
//   GET  /api/ludus/economy-graphs         workspace のキャッシュ済み graph 一覧
//   GET  /ludus/economy-graph/:slug        HTML (native/web/api/economy-graph.html、vis-network CDN)
// TS は ANTHROPIC_API_KEY を受けて分析していた。C++ は LLMClient (claude -p) を受ける (ludus/economy_analyzer.hpp)。
#include <cstddef>
#include <functional>
#include <string>

#include "infra/http.hpp"
#include "ludus/economy_analyzer.hpp"

namespace di::api {

struct EconomyGraphRoutesDeps {
    std::string workspaceId;
    /** 分析の依存 (LLM・DB・KG)。 */
    ludus::AnalyzeEconomyDeps analyze;
    /** 分析の開始 (テスト用の差し替え。既定 ludus::analyze_economy_in_background)。 */
    std::function<bool(const std::string& workspace_id, const std::string& game_title, bool force)> startAnalysis;
};

/** POST 本文の上限。 */
inline constexpr std::size_t kEconomyAnalyzeMaxBody = 16 * 1024;
/** gameTitle の上限 (UTF-16 単位。移植で追加。超えたら 400)。 */
inline constexpr std::size_t kEconomyGameTitleMax = 500;

/** HTML ページ (slug / apiBase を <script> 内で安全な JSON 文字列として差し込む)。 */
std::string build_economy_graph_html(const std::string& slug, const std::string& api_base);

/** <script> 内に置いても閉じタグ等にならない JSON 文字列 (< > & U+2028 U+2029 を \\u エスケープ)。 */
std::string script_safe_json_string(const std::string& value);

http::Router create_economy_graph_routes(EconomyGraphRoutesDeps deps);

} // namespace di::api
