#include "api/gap_creation_routes.hpp"

#include "base/sqlite.hpp"
#include "base/strings.hpp"
#include "config.hpp"
#include "core/index.hpp"
#include "core/kg_registry.hpp"
#include "discussion_seed/seed.hpp"
#include "middleware/auth.hpp"
#include "persona_engine/db/migrations.hpp"
#include "persona_engine/db/personas_repo.hpp"

namespace di::api {

std::optional<GapCreationInput> parse_gap_creation_input(const std::optional<Json>& body) {
    if (!body || !body->is_object()) return std::nullopt;
    const auto& input = *body;
    const auto non_blank = [&](const char* key) -> std::optional<std::string> {
        const auto it = input.find(key);
        if (it == input.end() || !it->is_string()) return std::nullopt;
        auto t = str::trim(it->get<std::string>());
        if (t.empty()) return std::nullopt;
        return t;
    };
    auto title = non_blank("title");
    if (!title) return std::nullopt;
    auto description = non_blank("description");
    if (!description) return std::nullopt;
    GapCreationInput out{*title, *description, std::nullopt};
    if (input.contains("gameId")) {
        auto g = non_blank("gameId");
        if (!g) return std::nullopt;
        out.gameId = *g;
    }
    return out;
}

http::Router gap_creation_routes() {
    http::Router router;
    router.post("/admin/gaps", [](http::Request& c) {
        if (auto guard = middleware::require_admin(c)) return *guard;
        // content-type の ";" より前を trim + 小文字化して比べる。
        const auto ct = c.header("content-type");
        std::optional<std::string> media;
        if (ct) media = str::to_lower(str::trim(ct->substr(0, ct->find(';'))));
        if (media != std::optional<std::string>("application/json"))
            return http::Response::json(Json{{"error", "content-type must be application/json"}}, 415);

        const auto input = parse_gap_creation_input(c.json());
        if (!input)
            return http::Response::json(Json{{"error", "title and description are required; gameId must be non-empty when supplied"}}, 400);

        const auto& config = config::get_config();
        try {
            core::CoreOptions opts;
            opts.sqlitePath = core::resolve_active_kg_path(config);
            auto core = core::create_core(opts);
            if (input->gameId &&
                !core->raw().get("SELECT 1 FROM games WHERE workspace_id = ? AND id = ?", {config.workspace, *input->gameId}))
                return http::Response::json(Json{{"error", "gameId must identify a game in the current workspace"}}, 400);

            auto persona_db = sql::Database::open(config.personaEngine.dbPath);
            persona_engine::db::apply_persona_engine_migrations(*persona_db);
            persona_engine::db::PersonasRepo personas(persona_db);
            discussion_seed::SeedHeadlessDiscussionArgs args;
            args.core = core.get();
            args.personas = &personas;
            args.workspaceId = config.workspace;
            args.topic = {input->title, input->description};
            args.gameId = input->gameId;
            args.origin = "api:admin-gap";
            return http::Response::json(discussion_seed::to_json(discussion_seed::seed_headless_discussion(args)), 201);
        } catch (const std::exception&) {
            // Database errors can contain local paths and SQL details; keep them out of the response.
            return http::Response::json(Json{{"error", "gap creation unavailable"}}, 503);
        }
    });
    return router;
}

} // namespace di::api
