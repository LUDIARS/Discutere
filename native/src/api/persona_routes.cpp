// @spec インポート (Vo → Di)
#include "api/persona_routes.hpp"

#include <algorithm>
#include <cmath>

#include "api/config_file_patch.hpp"
#include "api/web_page.hpp"
#include "base/env.hpp"
#include "base/strings.hpp"
#include "config.hpp"
#include "flow/persona_import.hpp"
#include "flow/persona_import_document.hpp"
#include "flow/persona_pool.hpp"

namespace di::api {

namespace {

http::Response internal_error() { return http::Response::text("Internal Server Error", 500); }

/** (await c.req.json().catch(() => ({}))) — 壊れた本文は {}。JSON の null は TS では TypeError (500) なので nullopt。 */
std::optional<Json> read_body(const http::Request& request) {
    auto parsed = request.json();
    if (!parsed) return Json::object();
    if (parsed->is_null()) return std::nullopt;
    return parsed;
}

const Json* field(const Json& body, const char* key) {
    if (!body.is_object()) return nullptr;
    auto it = body.find(key);
    return it == body.end() ? nullptr : &*it;
}

bool bool_value(const Json* value, bool fallback) {
    if (!value) return fallback;
    if (value->is_boolean()) return value->get<bool>();
    if (value->is_string()) {
        const auto v = str::to_lower_ascii(str::trim(value->get<std::string>()));
        return v == "1" || v == "true" || v == "yes" || v == "on";
    }
    return fallback;
}

/** JS の Number(value) (undefined は NaN)。 */
double js_number_of(const Json* value) {
    const double nan = std::nan("");
    if (!value) return nan;
    if (value->is_null()) return 0;
    if (value->is_boolean()) return value->get<bool>() ? 1 : 0;
    if (value->is_number()) return value->get<double>();
    if (value->is_string()) return str::js_number(value->get<std::string>()).value_or(nan);
    if (value->is_array()) {
        if (value->empty()) return 0;
        if (value->size() == 1 && ((*value)[0].is_string() || (*value)[0].is_number())) return js_number_of(&(*value)[0]);
    }
    return nan;
}

int bounded_integer(const Json* value, int fallback, int min, int max) {
    const double number = (value && value->is_number()) ? value->get<double>() : js_number_of(value);
    if (!std::isfinite(number)) return fallback;
    return static_cast<int>(std::max<double>(min, std::min<double>(max, std::floor(number))));
}

Json env_override() {
    const auto v = env::get("DISCUTERE_FLOW_AUTO_ADOPT_ON_CRAWL");
    return v ? Json(*v) : Json(nullptr);
}

/**
 * TS は書き込み後に _resetConfig() で getConfig() のキャッシュを捨てる。C++ の get_config() は参照を返すため
 * 実行中に捨てられない (他スレッドの参照が宙に浮く)。応答は config を読み直した値 (load_config) を返す。
 */
bool current_auto_adopt() { return config::load_config().flow.autoAdoptOnCrawl; }

} // namespace

http::Router persona_routes(PersonaRouteDeps deps) {
    if (!deps.runAdopt) deps.runAdopt = [](const flow::AdoptRunOptions& o) { return flow::run_adopt_from_kg(o); };
    http::Router routes;

    routes.get("/admin/personas", [](http::Request&) { return http::Response::html(std::string(web_page("api/personas.html"))); });

    routes.get("/admin/personas/data", [](http::Request&) {
        flow::PoolPersonaFilter imported;
        imported.origin = "imported";
        flow::PoolPersonaFilter adopted;
        adopted.origin = "adopted";
        return http::Response::json(Json{{"autoAdoptOnCrawl", current_auto_adopt()},
                                         {"envOverride", env_override()},
                                         {"configPath", config_file_path()},
                                         {"imported", flow::list_pool_personas(imported).size()},
                                         {"adopted", flow::list_pool_personas(adopted).size()}});
    });

    routes.put("/admin/personas/auto-adopt", [](http::Request& req) {
        const auto body = read_body(req);
        if (!body) return internal_error();
        const Json* enabled = field(*body, "enabled");
        if (!enabled) return http::Response::json(Json{{"error", "enabled required"}}, 400);
        const bool value = bool_value(enabled, false);
        try {
            patch_config_file([&](Json& current) {
                Json flow = Json::object();
                if (const Json* f = field(current, "flow"); f && f->is_object()) flow = *f;
                flow["autoAdoptOnCrawl"] = value;
                current["flow"] = std::move(flow);
            });
        } catch (const std::exception& e) {
            return http::Response::json(Json{{"error", std::string("config write failed: ") + e.what()}}, 500);
        }
        return http::Response::json(Json{{"ok", true},
                                         {"autoAdoptOnCrawl", current_auto_adopt()},
                                         {"envOverride", env_override()},
                                         {"configPath", config_file_path()}});
    });

    // 本文は JSON 配列 / { personas: [] } / NDJSON のいずれでもよい。CLI と同じ parse_persona_document で解釈し、
    // 壊れた行は落とさず skip 件数として返す。
    routes.post(
        "/admin/personas/import",
        [](http::Request& req) {
            try {
                const auto document = flow::parse_persona_document(req.body);
                flow::ImportVoluptasOptions opts;
                opts.invalidJsonLines = document.invalidJsonLines;
                Json personas = Json::array();
                for (const auto& p : document.personas) personas.push_back(p);
                Json out = Json{{"ok", true}};
                const Json summary = flow::to_json(flow::import_voluptas_personas(personas, opts));
                for (auto it = summary.begin(); it != summary.end(); ++it) out[it.key()] = it.value();
                return http::Response::json(out);
            } catch (const std::exception& e) {
                return http::Response::json(Json{{"error", e.what()}}, 400);
            }
        },
        http::RouteOptions{kPersonaImportMaxBody});

    routes.post("/admin/personas/adopt", [deps](http::Request& req) {
        const auto body = read_body(req);
        if (!body) return internal_error();
        std::optional<std::string> source_filter;
        if (const Json* s = field(*body, "source"); s && s->is_string()) {
            const auto t = str::trim(s->get<std::string>());
            if (!t.empty()) source_filter = t;
        }
        const int min_opinions = bounded_integer(field(*body, "minOpinions"), 10, 1, 10'000);
        const bool dry = bool_value(field(*body, "dry"), true);
        try {
            flow::AdoptRunOptions opts;
            opts.sourceFilter = source_filter;
            opts.minOpinions = min_opinions;
            opts.dry = dry;
            const auto summary = deps.runAdopt(opts);
            Json out = Json{{"ok", true},
                            {"dry", dry},
                            {"sourceFilter", source_filter ? Json(*source_filter) : Json(nullptr)},
                            {"minOpinions", min_opinions}};
            const Json summary_json = flow::to_json(summary);
            for (auto it = summary_json.begin(); it != summary_json.end(); ++it) out[it.key()] = it.value();
            return http::Response::json(out);
        } catch (const std::exception& e) {
            return http::Response::json(Json{{"error", e.what()}}, 500);
        }
    });
    return routes;
}

} // namespace di::api
