#include "api/datasource_routes.hpp"

#include <algorithm>
#include <cmath>
#include <memory>

#include "api/config_file_patch.hpp"
#include "base/strings.hpp"
#include "config.hpp"
#include "core/index.hpp"
#include "core/kg_registry.hpp"
#include "core/noise/exclusions.hpp"
#include "crawler/sources/attribution_store.hpp"
#include "crawler/sources/ingested_store.hpp"
#include "crawler/sources/persona.hpp"
#include "middleware/auth.hpp"

namespace di::api {

namespace {

/** source 単位の除外で列挙する上限 (TS と同じ)。 */
constexpr double kExcludeListLimit = 100'000;

template <class T>
struct CloseOnExit {
    T* p;
    ~CloseOnExit() {
        try {
            if (p) p->close();
        } catch (...) {
        }
    }
};

std::unique_ptr<core::Core> open_core() {
    core::CoreOptions co;
    co.sqlitePath = core::resolve_active_kg_path(config::get_config());
    return core::create_core(co);
}

Json opt_json(const std::optional<std::string>& v) { return v ? Json(*v) : Json(nullptr); }

/** c.req.json().catch(() => ({})) の文字列フィールド (truthy な文字列だけ)。 */
std::optional<std::string> body_string(const Json& body, const char* key) {
    if (!body.is_object()) return std::nullopt;
    auto it = body.find(key);
    if (it == body.end() || !it->is_string() || it->get<std::string>().empty()) return std::nullopt;
    return it->get<std::string>();
}

} // namespace

http::Router datasource_routes() {
    http::Router routes;

    // active KG + 宣言済み KG 一覧 + source 別件数 (source_attribution 集計)。
    routes.get("/admin/datasources", [](http::Request& req) {
        if (auto guard = middleware::require_admin(req)) return *guard;
        const auto& config = config::get_config();
        auto attribution = crawler::sources::open_attribution_store();
        CloseOnExit<crawler::sources::AttributionStore> close_attr{attribution.get()};
        Json sources = Json::array();
        for (const auto& s : attribution->counts_by_source()) sources.push_back(crawler::sources::to_json(s));
        Json kgs = Json::array();
        for (const auto& kg : core::list_knowledge_graphs(config)) kgs.push_back(core::to_json(kg));
        return http::Response::json(
            Json{{"activeKg", config.discatier.activeKg.value_or("default")}, {"knowledgeGraphs", std::move(kgs)}, {"sources", std::move(sources)}});
    });

    // 当該 source の取り込み発話一覧 (本文抜粋 + ペルソナ名 + 出所 + 生 ID は admin のみ)。
    routes.get("/admin/datasources/:source/utterances", [](http::Request& req) {
        if (auto guard = middleware::require_admin(req)) return *guard;
        const std::string source = req.param("source");
        const auto game_slug_q = req.query("gameSlug");
        const std::optional<std::string> game_slug =
            (game_slug_q && !game_slug_q->empty()) ? game_slug_q : std::nullopt;
        // Math.min(500, Math.max(1, Number(limit ?? 200))) — 数でない値は既定 200 (TS は NaN を SQL に渡していた)。
        double limit = 200;
        if (const auto l = req.query("limit")) limit = str::js_number(*l).value_or(200);
        if (!std::isfinite(limit)) limit = 200;
        limit = std::min(500.0, std::max(1.0, std::floor(limit)));
        auto core = open_core();
        CloseOnExit<core::Core> close_core{core.get()};
        auto attribution = crawler::sources::open_attribution_store();
        CloseOnExit<crawler::sources::AttributionStore> close_attr{attribution.get()};
        auto& raw = core->raw();
        const auto excluded = core::noise::list_excluded_ids(raw);
        Json items = Json::array();
        for (const auto& a : attribution->list_by_source(source, game_slug, limit)) {
            const auto u = raw.get("SELECT speaker_id, raw_content FROM utterances WHERE id = ?", {a.utteranceId});
            const std::string content = u ? u->str("raw_content") : std::string();
            const auto speaker_id = u ? u->opt_str("speaker_id") : std::nullopt;
            Json item = Json::object();
            item["utteranceId"] = a.utteranceId;
            // §6: 個人はペルソナ名へマスク、出所 (source/sourceUrl) は開示。
            item["speaker"] = (speaker_id && !speaker_id->empty()) ? crawler::sources::masked_persona_label(*speaker_id) : "外部の声";
            item["excerpt"] = str::js_length(content) > 280 ? str::js_slice(content, 0, 280) + "…" : content;
            item["source"] = a.source;
            item["sourceUrl"] = a.sourceUrl;
            item["gameSlug"] = opt_json(a.gameSlug);
            // 生 ID は admin のみ可視 (本ルートは require_admin 済)。
            item["speakerId"] = opt_json(speaker_id);
            item["nativeId"] = a.nativeId;
            item["importedAt"] = a.importedAt;
            item["excluded"] = excluded.count(a.utteranceId) > 0;
            items.push_back(std::move(item));
        }
        return http::Response::json(Json{{"source", source}, {"gameSlug", opt_json(game_slug)}, {"items", std::move(items)}});
    });

    // 発話単位 or (source[, gameSlug]) 単位で取り込みを除外する。.ingested 台帳に印を残し再取り込みを防ぐ。
    routes.post("/admin/datasources/exclude", [](http::Request& req) {
        if (auto guard = middleware::require_admin(req)) return *guard;
        auto parsed = req.json();
        if (parsed && parsed->is_null()) return http::Response::text("Internal Server Error", 500);
        const Json body = parsed ? *parsed : Json::object();
        const auto utterance_id = body_string(body, "utteranceId");
        const auto source = body_string(body, "source");
        if (!utterance_id && !source) return http::Response::json(Json{{"error", "utteranceId or source required"}}, 400);
        std::optional<std::string> game_slug;
        if (body.is_object())
            if (auto it = body.find("gameSlug"); it != body.end() && it->is_string()) game_slug = it->get<std::string>();
        auto core = open_core();
        CloseOnExit<core::Core> close_core{core.get()};
        auto attribution = crawler::sources::open_attribution_store();
        CloseOnExit<crawler::sources::AttributionStore> close_attr{attribution.get()};
        auto ingested = crawler::sources::open_ingested_store();
        CloseOnExit<crawler::sources::IngestedStore> close_ing{ingested.get()};
        auto& raw = core->raw();
        std::vector<crawler::sources::Attribution> targets;
        if (utterance_id) {
            if (auto a = attribution->get(*utterance_id)) targets.push_back(*a);
            else {
                crawler::sources::Attribution synthetic;
                synthetic.utteranceId = *utterance_id;
                targets.push_back(synthetic);
            }
        } else {
            targets = attribution->list_by_source(*source, game_slug, kExcludeListLimit);
        }
        const std::string reason = source ? "datasource:" + *source : "datasource";
        int excluded_count = 0;
        for (const auto& a : targets) {
            core::noise::exclude_utterance(raw, a.utteranceId, reason);
            if (!a.source.empty() && !a.nativeId.empty()) ingested->add(a.source, a.nativeId);
            ++excluded_count;
        }
        return http::Response::json(Json{{"ok", true}, {"excluded", excluded_count}});
    });

    // 次に開く KG (activeKg) を config ファイルに書き戻す。即時切替はせず再起動が必要 (§12)。
    routes.post("/admin/datasources/active-kg", [](http::Request& req) {
        if (auto guard = middleware::require_admin(req)) return *guard;
        auto parsed = req.json();
        if (parsed && parsed->is_null()) return http::Response::text("Internal Server Error", 500);
        const auto id = body_string(parsed ? *parsed : Json::object(), "id");
        if (!id) return http::Response::json(Json{{"error", "id required"}}, 400);
        std::vector<std::string> known;
        for (const auto& kg : core::list_knowledge_graphs(config::get_config())) known.push_back(kg.id);
        if (!known.empty() && std::find(known.begin(), known.end(), *id) == known.end())
            return http::Response::json(Json{{"error", "unknown KG id: " + *id}, {"knownIds", known}}, 400);
        try {
            patch_config_file([&](Json& current) {
                Json discatier = Json::object();
                if (auto it = current.find("discatier"); it != current.end() && it->is_object()) discatier = *it;
                discatier["activeKg"] = *id;
                current["discatier"] = std::move(discatier);
            });
        } catch (const std::exception& e) {
            return http::Response::json(Json{{"error", std::string("config 書き込み失敗: ") + e.what()}}, 500);
        }
        return http::Response::json(Json{{"ok", true}, {"activeKg", *id}, {"restartRequired", true}});
    });

    return routes;
}

} // namespace di::api
