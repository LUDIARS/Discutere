#include "api/consensus_routes.hpp"

#include <string>

#include "api/consensus_persona_routes.hpp"
#include "api/gap_creation_routes.hpp"
#include "api/web_page.hpp"
#include "base/json.hpp"
#include "base/sqlite.hpp"
#include "base/strings.hpp"
#include "config.hpp"
#include "core/kg_registry.hpp"
#include "middleware/auth.hpp"
#include "persona_engine/facilitator/facilitator.hpp"

namespace di::api {

namespace {

/** DB から読む行の上限 (DB の値も外部入力として課金する)。 */
constexpr sql::QueryLimits kRowLimits{200'000, 256u << 20};

std::int64_t scored_count(core::Core& core, const std::string& session_id) {
    const auto row = core.raw().get(
        R"(SELECT COUNT(*) AS n FROM consensus_scores
        WHERE utterance_id IN (SELECT id FROM utterances WHERE session_id = ?))",
        {session_id});
    return row ? row->i64("n") : 0;
}

bool is_human_speaker(const std::optional<std::string>& speaker_id) {
    return speaker_id && !speaker_id->empty() && !str::starts_with(*speaker_id, "persona:") && !str::starts_with(*speaker_id, "ext:");
}

std::string speaker_label(const std::string& speaker_id) {
    // personas テーブルは persona-engine DB 側にあり KG (core) には無いので、
    // conclusions と同じく persona id をそのままラベルにする (人間は「人間」)。
    if (str::starts_with(speaker_id, "persona:")) return speaker_id.substr(std::string_view("persona:").size());
    return "人間";
}

Json opinions_for(core::Core& core, const std::string& session_id) {
    const auto rows = core.raw().all(
        R"(SELECT u.id AS id, u.speaker_id AS speakerId, u.raw_content AS content,
              cs.agree AS agree, cs.score AS score, cs.reasoning AS reasoning
         FROM utterances u
         LEFT JOIN consensus_scores cs ON cs.utterance_id = u.id
        WHERE u.session_id = ?
          AND u.speaker_id IS NOT NULL
          AND u.speaker_id NOT LIKE 'ext:%'
          AND u.speaker_id <> ?
        ORDER BY COALESCE(cs.score, -1) DESC, u.posted_at ASC)",
        {session_id, std::string("persona:") + persona_engine::facilitator::FACILITATOR_PERSONA_ID}, kRowLimits);
    Json out = Json::array();
    for (const auto& r : rows) {
        const auto content = r.str("content");
        if (str::starts_with(content, "【収束】")) continue;
        const auto speaker = r.opt_str("speakerId");
        Json o = Json::object();
        o["utteranceId"] = r["id"].to_json();
        o["speaker"] = speaker_label(speaker.value_or(""));
        o["human"] = is_human_speaker(speaker);
        o["content"] = r["content"].to_json();
        o["agree"] = r.is_null("agree") ? Json(nullptr) : Json(r["agree"].is_number() && r["agree"].as_double() == 1);
        o["score"] = r["score"].to_json();
        o["reasoning"] = r["reasoning"].to_json();
        out.push_back(std::move(o));
    }
    return out;
}

} // namespace

std::unique_ptr<core::Core> open_active_core() {
    core::CoreOptions opts;
    opts.sqlitePath = core::resolve_active_kg_path(config::get_config());
    return core::create_core(opts);
}

http::Router consensus_routes(CoreOpener open_core) {
    if (!open_core) open_core = open_active_core;
    http::Router router;
    router.mount("/", consensus_persona_routes());
    router.mount("/", gap_creation_routes());

    router.get("/admin/consensus", [](http::Request& c) {
        if (auto guard = middleware::require_admin(c)) return *guard;
        return http::Response::html(std::string(web_page("api/consensus.html")));
    });

    router.get("/admin/consensus/gaps", [open_core](http::Request& c) {
        if (auto guard = middleware::require_admin(c)) return *guard;
        const auto ws = config::get_config().workspace;
        auto core = open_core();
        const auto rows = core->raw().all(
            R"(SELECT g.id AS gapId, g.title AS title, g.status AS status, g.updated_at AS updatedAt, s.id AS sessionId
           FROM design_gaps g
           JOIN sessions s
             ON s.workspace_id = g.workspace_id
            AND s.title = 'discussion-of-gap:' || g.id
          WHERE g.workspace_id = ?
          ORDER BY g.updated_at DESC
          LIMIT 50)",
            {ws}, kRowLimits);
        Json gaps = Json::array();
        for (const auto& r : rows) {
            Json g = Json::object();
            g["gapId"] = r["gapId"].to_json();
            g["sessionId"] = r["sessionId"].to_json();
            g["title"] = r["title"].to_json();
            g["status"] = r["status"].to_json();
            g["scoredCount"] = scored_count(*core, r.str("sessionId"));
            gaps.push_back(std::move(g));
        }
        core->close();
        return http::Response::json(Json{{"gaps", gaps}});
    });

    router.get("/admin/consensus/:gapId", [open_core](http::Request& c) {
        if (auto guard = middleware::require_admin(c)) return *guard;
        const auto ws = config::get_config().workspace;
        const auto gap_id = c.param("gapId");
        auto core = open_core();
        const auto session = core->raw().get(
            "SELECT id FROM sessions WHERE workspace_id = ? AND title = ? ORDER BY started_at DESC LIMIT 1",
            {ws, "discussion-of-gap:" + gap_id});
        Json body = Json::object();
        body["gapId"] = gap_id;
        body["opinions"] = session ? opinions_for(*core, session->str("id")) : Json::array();
        core->close();
        return http::Response::json(body);
    });

    return router;
}

} // namespace di::api
