#include "api/noise_routes.hpp"

#include <memory>

#include "api/web_page.hpp"
#include "base/strings.hpp"
#include "config.hpp"
#include "core/index.hpp"
#include "core/kg_registry.hpp"
#include "core/noise/exclusions.hpp"
#include "middleware/auth.hpp"

namespace di::api {

namespace {

/** 一覧の行・バイトの上限 (DB の値も外部入力として課金する)。 */
constexpr sql::QueryLimits kNoiseListLimits{200'000, 256u << 20};

/** active KG を開き、スコープを抜けるときに閉じる。 */
struct ActiveCore {
    std::unique_ptr<core::Core> core;
    ActiveCore() {
        core::CoreOptions co;
        co.sqlitePath = core::resolve_active_kg_path(config::get_config());
        core = core::create_core(co);
    }
    ~ActiveCore() {
        try {
            core->close();
        } catch (...) {
        }
    }
    sql::Database& raw() { return core->raw(); }
};

/** persona:<id> / ext:<...> / 人間 を人間可読ラベルに解決する。 */
std::string speaker_label(sql::Database& raw, const std::optional<std::string>& speaker_id) {
    if (!speaker_id || speaker_id->empty()) return "参加者";
    if (str::starts_with(*speaker_id, "persona:")) {
        const std::string id = speaker_id->substr(8);
        const auto p = raw.get("SELECT display_name FROM personas WHERE id = ?", {id});
        if (p && !p->is_null("display_name")) return p->str("display_name");
        return id;
    }
    if (str::starts_with(*speaker_id, "ext:")) return "外部の声";
    return "人間";
}

/** (await c.req.json().catch(() => ({}))) の utteranceId / reason。null 本文は TS では TypeError (500)。 */
struct NoiseBody {
    bool null_body = false;
    std::optional<std::string> utteranceId;
    std::optional<std::string> reason;
};

NoiseBody read_noise_body(const http::Request& req) {
    NoiseBody out;
    auto parsed = req.json();
    if (!parsed) return out;
    if (parsed->is_null()) {
        out.null_body = true;
        return out;
    }
    if (!parsed->is_object()) return out;
    if (auto it = parsed->find("utteranceId"); it != parsed->end() && it->is_string() && !it->get<std::string>().empty())
        out.utteranceId = it->get<std::string>();
    if (auto it = parsed->find("reason"); it != parsed->end() && it->is_string()) out.reason = it->get<std::string>();
    return out;
}

} // namespace

http::Router noise_routes() {
    http::Router routes;

    routes.get("/admin/noise", [](http::Request& req) {
        if (auto guard = middleware::require_admin(req)) return *guard;
        return http::Response::html(std::string(web_page("api/noise.html")));
    });

    // 議論 session 一覧 (gap title + 発話数) と ソース種別 (title prefix) を返す。
    routes.get("/admin/noise/groups", [](http::Request& req) {
        if (auto guard = middleware::require_admin(req)) return *guard;
        const std::string ws = config::get_config().workspace;
        ActiveCore core;
        auto& raw = core.raw();
        core::noise::ensure_exclusion_table(raw);
        const auto sess_rows = raw.all(
            "SELECT s.id AS sessionId, s.title AS title,\n"
            "       (SELECT COUNT(*) FROM utterances u WHERE u.session_id = s.id) AS utteranceCount\n"
            "  FROM sessions s\n"
            " WHERE s.workspace_id = ? AND s.title LIKE 'discussion-of-gap:%'\n"
            " ORDER BY utteranceCount DESC",
            {ws}, kNoiseListLimits);
        Json sessions = Json::array();
        for (const auto& r : sess_rows) {
            const std::string title = r.str("title");
            const std::string gap_id = str::js_slice(title, 18);  // "discussion-of-gap:".length
            const auto g = raw.get("SELECT title FROM design_gaps WHERE id = ?", {gap_id});
            sessions.push_back(Json{{"sessionId", r["sessionId"].to_json()},
                                    {"gapId", gap_id},
                                    {"title", (g && !g->is_null("title")) ? Json(g->str("title")) : r["title"].to_json()},
                                    {"utteranceCount", r["utteranceCount"].to_json()}});
        }
        // ソース種別: title の `<source>:<slug>` の source 部分で粗くグルーピング。
        const auto src_rows = raw.all(
            "SELECT SUBSTR(title, 1, INSTR(title, ':') - 1) AS source, COUNT(*) AS count\n"
            "  FROM sessions\n"
            " WHERE workspace_id = ? AND INSTR(title, ':') > 0\n"
            " GROUP BY source\n"
            " ORDER BY count DESC",
            {ws}, kNoiseListLimits);
        Json sources = Json::array();
        for (const auto& r : src_rows) sources.push_back(r.to_json());
        return http::Response::json(Json{{"sessions", std::move(sessions)}, {"sources", std::move(sources)}});
    });

    // session または source の発話一覧 (除外フラグ付き)。
    routes.get("/admin/noise/utterances", [](http::Request& req) {
        if (auto guard = middleware::require_admin(req)) return *guard;
        const std::string ws = config::get_config().workspace;
        const auto session_id = req.query("sessionId");
        const auto source = req.query("source");
        const bool has_session = session_id && !session_id->empty();
        const bool has_source = source && !source->empty();
        if (!has_session && !has_source) return http::Response::json(Json{{"error", "sessionId or source required"}}, 400);
        ActiveCore core;
        auto& raw = core.raw();
        const auto excluded = core::noise::list_excluded_ids(raw);
        std::vector<sql::Row> rows;
        if (has_session) {
            rows = raw.all("SELECT id, speaker_id, raw_content, posted_at FROM utterances WHERE session_id = ? ORDER BY posted_at ASC LIMIT 300",
                           {*session_id}, kNoiseListLimits);
        } else {
            // source: title が `<source>:` で始まる session 群の発話を集約。
            rows = raw.all(
                "SELECT u.id, u.speaker_id, u.raw_content, u.posted_at\n"
                "  FROM utterances u\n"
                "  JOIN sessions s ON s.id = u.session_id\n"
                " WHERE s.workspace_id = ? AND s.title LIKE ?\n"
                " ORDER BY u.posted_at ASC LIMIT 300",
                {ws, *source + ":%"}, kNoiseListLimits);
        }
        Json out = Json::array();
        for (const auto& u : rows) {
            const std::string id = u.str("id");
            out.push_back(Json{{"id", u["id"].to_json()},
                               {"speaker", speaker_label(raw, u.opt_str("speaker_id"))},
                               {"content", u["raw_content"].to_json()},
                               {"posted_at", u["posted_at"].to_json()},
                               {"excluded", excluded.count(id) > 0}});
        }
        return http::Response::json(out);
    });

    routes.post("/admin/noise/exclude", [](http::Request& req) {
        if (auto guard = middleware::require_admin(req)) return *guard;
        const auto body = read_noise_body(req);
        if (body.null_body) return http::Response::text("Internal Server Error", 500);
        if (!body.utteranceId) return http::Response::json(Json{{"error", "utteranceId required"}}, 400);
        ActiveCore core;
        core::noise::exclude_utterance(core.raw(), *body.utteranceId, body.reason);
        return http::Response::json(Json{{"ok", true}});
    });

    routes.post("/admin/noise/restore", [](http::Request& req) {
        if (auto guard = middleware::require_admin(req)) return *guard;
        const auto body = read_noise_body(req);
        if (body.null_body) return http::Response::text("Internal Server Error", 500);
        if (!body.utteranceId) return http::Response::json(Json{{"error", "utteranceId required"}}, 400);
        ActiveCore core;
        core::noise::restore_utterance(core.raw(), *body.utteranceId);
        return http::Response::json(Json{{"ok", true}});
    });

    return routes;
}

} // namespace di::api
