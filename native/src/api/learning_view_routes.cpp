#include "api/learning_view_routes.hpp"

#include <algorithm>
#include <limits>
#include <vector>

#include "api/js_query.hpp"
#include "api/web_page.hpp"
#include "base/json.hpp"
#include "base/strings.hpp"
#include "base/url.hpp"
#include "config.hpp"
#include "flow/db/connection.hpp"
#include "fundamentum_export/index.hpp"
#include "visualize/conclusion_cache.hpp"
#include "visualize/conclusion_markdown.hpp"
#include "visualize/conclusions.hpp"
#include "visualize/flow_conclusions.hpp"
#include "visualize/learning_cache.hpp"
#include "visualize/learning_layers.hpp"

namespace di::api {

namespace {

http::Response not_found() { return http::Response::json(Json{{"error", "not found"}}, 404); }

/**
 * Number(c.req.query(name) ?? fallback) を SQL の LIMIT に渡せる整数に。
 * TS は NaN / 小数をそのまま LIMIT に渡して SQLite が datatype mismatch (500) になっていたので、同じく nullopt (= 500)。
 */
std::optional<std::int64_t> limit_query(const http::Request& c, const char* name, std::int64_t fallback) {
    const auto q = c.query(name);
    if (!q) return fallback;
    const auto n = str::js_number(*q);
    if (!n) return std::nullopt;
    return js_query::to_int64(*n);
}

/** Array.prototype.slice(0, limit) (負の limit は末尾から落とす)。 */
template <class T>
void js_slice_head(std::vector<T>& v, std::int64_t limit) {
    const auto size = static_cast<std::int64_t>(v.size());
    std::int64_t end = limit < 0 ? std::max<std::int64_t>(0, size + limit) : std::min(size, limit);
    v.resize(static_cast<std::size_t>(end));
}

/** gap に応じて新フロー / 旧フローの論述データを引く (旧フローは KG + 出所メタ)。 */
std::optional<visualize::ConclusionDetail> load_detail(const LearningViewDeps& deps, const std::string& gap_id) {
    if (visualize::is_flow_conclusion_id(gap_id)) {
        auto db = deps.flowDb();
        return visualize::get_flow_conclusion_detail(*db, visualize::flow_session_id_from_gap_id(gap_id));
    }
    const auto& config = config::get_config();
    auto core = deps.openCore();
    // 出所メタ (source/sourceUrl) を発話に付ける (§6 露出制御。個人マスクは serializer 側)。
    auto attribution = deps.openAttribution();
    struct Closer {
        core::Core* core;
        crawler::sources::AttributionStore* attribution;
        ~Closer() {
            try {
                attribution->close();
            } catch (...) {
            }
            core->close();
        }
    } closer{core.get(), attribution.get()};
    return visualize::get_conclusion_detail(*core, config.workspace, gap_id, attribution.get());
}

} // namespace

http::Router learning_view_routes(LearningViewDeps deps) {
    if (!deps.openCore) deps.openCore = open_active_core;
    if (!deps.flowDb) deps.flowDb = [] { return flow::db::get_flow_db(); };
    if (!deps.openAttribution) deps.openAttribution = [] { return crawler::sources::open_attribution_store(); };

    http::Router router;

    router.get("/learning", [](http::Request&) { return http::Response::html(std::string(web_page("api/learning.html"))); });

    // Gap率 (運営想定 vs 観測) は事前ビルドの cache から配信 (build:learning-cache で焼く)。
    router.get("/learning/gap", [deps](http::Request&) {
        if (!visualize::learning_cache_exists(deps.learningCachePath)) return http::Response::json(Json{{"games", Json::array()}});
        auto reader = visualize::open_learning_cache_reader(deps.learningCachePath);
        auto body = reader->gap_summary();
        reader->close();
        return http::Response::json(body);
    });

    router.get("/learning/gap/detail", [deps](http::Request& c) {
        if (!visualize::learning_cache_exists(deps.learningCachePath)) return http::Response::json(Json{{"error", "no cache"}}, 404);
        auto reader = visualize::open_learning_cache_reader(deps.learningCachePath);
        const auto detail = reader->gap(c.query("slug").value_or(""));
        reader->close();
        return detail ? http::Response::json(*detail) : not_found();
    });

    // データソース構成 (どの取得元が何件か) は cache から配信 (build:learning-cache で焼く)。
    router.get("/learning/sources", [deps](http::Request&) {
        if (!visualize::learning_cache_exists(deps.learningCachePath)) {
            Json body = Json::object();
            body["totals"] = Json{{"sources", 0}};
            body["sources"] = Json::array();
            body["byGame"] = Json::array();
            return http::Response::json(body);
        }
        auto reader = visualize::open_learning_cache_reader(deps.learningCachePath);
        auto body = reader->data_sources();
        reader->close();
        return http::Response::json(body);
    });

    // 収束した議論の結論一覧 (#66 — Learning View 統合)。
    // 既定は SQLite キャッシュ (conclusion_cache) から読む = KG 非タッチで高速
    // (議論収束ごとに write-through 更新 + build:conclusion-cache で材料件数算出)。
    // キャッシュ未構築 (0 件) のときだけ従来の live マージ (KG 直読) にフォールバックする。
    router.get("/learning/conclusions", [deps](http::Request& c) {
        const auto limit = limit_query(c, "limit", 100);
        if (!limit) return http::Response::text("Internal Server Error", 500);
        auto flow_db = deps.flowDb();

        // 新フロー結論をキャッシュへ追いつかせる (KG 非依存・追いついていれば COUNT 2 回で即 return)。
        visualize::ensure_conclusion_cache_fresh(*flow_db);

        if (visualize::conclusion_cache_count(*flow_db) > 0) {
            Json list = Json::array();
            for (const auto& s : visualize::list_cached_conclusions(*flow_db, *limit)) list.push_back(visualize::to_json(s));
            return http::Response::json(Json{{"conclusions", list}, {"cached", true}});
        }

        // フォールバック: キャッシュ未構築。live で KG + flow をマージ (重い)。
        const auto& config = config::get_config();
        auto core = deps.openCore();
        auto merged = visualize::list_conclusions(*core, config.workspace, *limit);
        core->close();
        auto flow_list = visualize::list_flow_conclusions(*flow_db, *limit);
        merged.insert(merged.end(), flow_list.begin(), flow_list.end());
        std::stable_sort(merged.begin(), merged.end(),
                         [](const visualize::ConclusionSummary& a, const visualize::ConclusionSummary& b) { return b.updatedAt < a.updatedAt; });
        js_slice_head(merged, *limit);
        Json list = Json::array();
        for (const auto& s : merged) list.push_back(visualize::to_json(s));
        return http::Response::json(Json{{"conclusions", list}, {"cached", false}});
    });

    // 1 件の結論の裏の論述データ (議論ログ / 止揚 / 高評価意見)。
    // gap="flow:<sessionId>" は新フロー、それ以外は旧フロー (design_gap) としてルートする。
    router.get("/learning/conclusion", [deps](http::Request& c) {
        const auto detail = load_detail(deps, c.query("gap").value_or(""));
        if (!detail) return not_found();
        return http::Response::json(visualize::to_json(*detail));
    });

    // 1 件の議論を単体 md ファイルとしてダウンロードする (議論 md エクスポート)。
    router.get("/learning/conclusion/export", [deps](http::Request& c) {
        const auto detail = load_detail(deps, c.query("gap").value_or(""));
        if (!detail) return not_found();
        auto md = visualize::render_conclusion_markdown(*detail);
        const auto filename = visualize::safe_slug(detail->title, detail->sessionId.empty() ? "discussion" : detail->sessionId) + ".md";
        http::Response r;
        r.status = 200;
        r.body = std::move(md);
        r.headers.set("Content-Type", "text/markdown; charset=utf-8");
        r.headers.set("Content-Disposition", "attachment; filename=\"discussion.md\"; filename*=UTF-8''" + url::encode_component(filename));
        return r;
    });

    // 1 件の議論を Fundamentum (議論データ管理ツール) へ個別 export する (議論データチューナー)。
    router.post("/learning/conclusion/export/fundamentum", [deps](http::Request& c) {
        const auto detail = load_detail(deps, c.query("gap").value_or(""));
        if (!detail) return not_found();
        return http::Response::json(fundamentum_export::to_json(fundamentum_export::export_discussion_to_fundamentum(*detail)));
    });

    router.get("/learning/data", [deps](http::Request& c) {
        const auto& config = config::get_config();
        const auto layer = visualize::normalize_learning_layer(c.query("layer"));
        visualize::LearningLayerOptions opts;
        // Number(v ?? 既定)。NaN は learning_layers の clampLimit が既定に倒す (TS と同じ)。
        const auto num = [&](const std::optional<std::string>& v, double fallback) -> std::optional<double> {
            if (!v) return fallback;
            const auto n = str::js_number(*v);
            return n ? *n : std::numeric_limits<double>::quiet_NaN();
        };
        opts.limit = num(c.query("limit"), 60);
        auto detail_q = c.query("detailLimit");
        if (!detail_q) detail_q = c.query("opinionsPerTopic");
        opts.detailLimit = num(detail_q, 8);
        auto core = deps.openCore();
        auto body = visualize::build_learning_layer_snapshot(core->raw(), config.workspace, layer, opts);
        core->close();
        return http::Response::json(body);
    });

    return router;
}

} // namespace di::api
