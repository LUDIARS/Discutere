#include "api/economy_graph_routes.hpp"

#include <map>
#include <memory>

#include "api/web_page.hpp"
#include "base/log.hpp"
#include "base/strings.hpp"
#include "base/time.hpp"
#include "db/repository.hpp"

namespace di::api {

std::string script_safe_json_string(const std::string& value) {
    const std::string raw = json_stringify(Json(str::sanitize_utf8(value)));
    std::string out;
    out.reserve(raw.size() + 16);
    for (std::size_t i = 0; i < raw.size(); ++i) {
        const char c = raw[i];
        if (c == '<') out += "\\u003c";
        else if (c == '>') out += "\\u003e";
        else if (c == '&') out += "\\u0026";
        else if (static_cast<unsigned char>(c) == 0xE2 && i + 2 < raw.size() && static_cast<unsigned char>(raw[i + 1]) == 0x80 &&
                 (static_cast<unsigned char>(raw[i + 2]) == 0xA8 || static_cast<unsigned char>(raw[i + 2]) == 0xA9)) {
            out += static_cast<unsigned char>(raw[i + 2]) == 0xA8 ? "\\u2028" : "\\u2029";
            i += 2;
        } else out += c;
    }
    return out;
}

std::string build_economy_graph_html(const std::string& slug, const std::string& api_base) {
    const std::string_view tpl = web_page("api/economy-graph.html");
    const std::map<std::string, std::string> values{{"slugJson", script_safe_json_string(slug)},
                                                    {"apiBaseJson", script_safe_json_string(api_base)}};
    // {{名前}} を 1 回の走査で置き換える (置き換えた値の中は再走査しない)。
    std::string out;
    out.reserve(tpl.size() + 256);
    std::size_t i = 0;
    while (i < tpl.size()) {
        const std::size_t open = tpl.find("{{", i);
        const std::size_t close = open == std::string_view::npos ? open : tpl.find("}}", open + 2);
        if (open == std::string_view::npos || close == std::string_view::npos) {
            out.append(tpl.substr(i));
            break;
        }
        out.append(tpl.substr(i, open - i));
        const auto it = values.find(std::string(tpl.substr(open + 2, close - open - 2)));
        if (it != values.end()) {
            out.append(it->second);
            i = close + 2;
        } else {
            out.append("{{");
            i = open + 2;
        }
    }
    return out;
}

http::Router create_economy_graph_routes(EconomyGraphRoutesDeps deps) {
    auto d = std::make_shared<EconomyGraphRoutesDeps>(std::move(deps));
    if (!d->startAnalysis) {
        d->startAnalysis = [d_weak = std::weak_ptr<EconomyGraphRoutesDeps>(d)](const std::string& ws, const std::string& title, bool force) {
            auto self = d_weak.lock();
            if (!self) return false;
            return ludus::analyze_economy_in_background(ws, title, self->analyze, force);
        };
    }
    auto graphs = [d] { return db::EconomyGraphRepo(d->analyze.mainDb).list_by_workspace(d->workspaceId); };
    http::Router app;

    app.post(
        "/api/ludus/analyze-economy",
        [d, graphs](http::Request& req) {
            // c.req.json().catch(() => ({})) — 壊れた本文は空 object 扱い。
            const Json body = req.json(JsonLimits{kEconomyAnalyzeMaxBody, 1000, 16}).value_or(Json::object());
            std::optional<std::string> game_title;
            bool force = false;
            if (body.is_object()) {
                if (body.contains("gameTitle") && body["gameTitle"].is_string() && !body["gameTitle"].get<std::string>().empty())
                    game_title = body["gameTitle"].get<std::string>();
                if (!game_title && body.contains("slug") && body["slug"].is_string() && !body["slug"].get<std::string>().empty()) {
                    const std::string slug = body["slug"].get<std::string>();
                    for (const auto& g : graphs())
                        if (ludus::to_slug(g.gameTitle) == slug) {
                            game_title = g.gameTitle;
                            break;
                        }
                }
                if (body.contains("force")) force = json_truthy(body["force"]);
            }
            if (!game_title || game_title->empty())
                return http::Response::json(Json{{"ok", false}, {"error", "gameTitle or slug required"}}, 400);
            if (str::js_length(*game_title) > kEconomyGameTitleMax)
                return http::Response::json(Json{{"ok", false}, {"error", "gameTitle too long"}}, 400);

            if (!d->startAnalysis(d->workspaceId, *game_title, force))
                log::warn("[economy-graph] analyze not started (busy or queue full): " + str::js_slice(*game_title, 0, 80));

            return http::Response::json(Json{{"ok", true}, {"slug", ludus::to_slug(*game_title)}, {"message", "analysis started"}});
        },
        http::RouteOptions{kEconomyAnalyzeMaxBody});

    app.get("/api/ludus/economy-graphs", [graphs](http::Request&) {
        Json arr = Json::array();
        for (const auto& g : graphs()) {
            arr.push_back(Json{{"slug", ludus::to_slug(g.gameTitle)},
                               {"gameTitle", g.gameTitle},
                               {"mechanicCount", g.mechanicCount},
                               {"updatedAt", time::iso_utc(g.updatedAt * 1000)}});
        }
        return http::Response::json(arr);
    });

    app.get("/api/ludus/economy-graph/:slug", [graphs](http::Request& req) {
        const std::string slug = req.param("slug");
        for (const auto& g : graphs()) {
            if (ludus::to_slug(g.gameTitle) != slug) continue;
            const auto parsed = parse_json(g.graphJson, JsonLimits{ludus::kEconomyGraphJsonMaxBytes, 200'000, 32});
            if (!parsed) return http::Response::text("Internal Server Error", 500);
            return http::Response::json(*parsed);
        }
        return http::Response::json(Json{{"error", "not found"}}, 404);
    });

    app.get("/ludus/economy-graph/:slug", [](http::Request& req) {
        const std::string slug = req.param("slug");
        const std::string proto = req.header("x-forwarded-proto").value_or("http");
        const std::string host = req.header("host").value_or("localhost");
        return http::Response::html(build_economy_graph_html(slug, proto + "://" + host));
    });

    return app;
}

} // namespace di::api
