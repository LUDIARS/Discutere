#include "api/consensus_persona_routes.hpp"

#include <map>

#include "api/consensus_persona_aggregation.hpp"
#include "base/json.hpp"
#include "base/sqlite.hpp"
#include "config.hpp"
#include "core/index.hpp"
#include "core/kg_registry.hpp"
#include "middleware/auth.hpp"
#include "persona_engine/facilitator/facilitator.hpp"

namespace di::api {

namespace {

constexpr sql::QueryLimits kRowLimits{200'000, 256u << 20};

/** traits 列 (JSON の string[])。壊れている・文字列以外を含むなら []。 */
std::vector<std::string> parse_traits(const std::string& raw) {
    const auto parsed = parse_json(raw, {64u << 10, 10'000, 8});
    std::vector<std::string> out;
    if (!parsed || !parsed->is_array()) return out;
    for (const auto& t : *parsed) {
        if (!t.is_string()) return {};
        out.push_back(t.get<std::string>());
    }
    return out;
}

/** @implements SPEC-THALEIA-PERSONA-CONSENSUS */
std::vector<PersonaConsensusSummary> load_persona_consensus(sql::Database& core_db, sql::Database& persona_db, const std::string& session_id) {
    const auto utterances = core_db.all(R"(SELECT u.id, u.speaker_id AS speakerId, u.raw_content AS content, u.posted_at AS postedAt,
              cs.agree, cs.score, cs.reasoning, COALESCE(os.score, 0) AS opinionScore
         FROM utterances u
         LEFT JOIN consensus_scores cs ON cs.utterance_id = u.id
         LEFT JOIN opinion_scores os ON os.target_id = u.id
        WHERE u.session_id = ?
          AND u.speaker_id LIKE 'persona:%'
          AND u.speaker_id NOT LIKE 'ext:%'
          AND u.speaker_id <> ?
        ORDER BY u.posted_at ASC)",
                                        {session_id, std::string("persona:") + persona_engine::facilitator::FACILITATOR_PERSONA_ID},
                                        kRowLimits);
    struct Persona {
        std::string display_name;
        std::string traits;
    };
    std::map<std::string, Persona> persona_by_id;
    for (const auto& p : persona_db.all("SELECT id, display_name, traits FROM personas", {}, kRowLimits))
        persona_by_id[p.str("id")] = {p.str("display_name"), p.str("traits")};

    std::vector<PersonaConsensusSource> rows;
    for (const auto& u : utterances) {
        const auto speaker = u.str("speakerId");
        const std::string persona_id = speaker.substr(std::string_view("persona:").size());
        const auto it = persona_by_id.find(persona_id);
        if (it == persona_by_id.end()) continue;
        PersonaConsensusSource s;
        s.id = u.str("id");
        s.content = u.str("content");
        s.postedAt = u.num("postedAt");
        if (!u.is_null("agree")) s.agree = u.i64("agree") == 1;
        s.score = u.opt_num("score");
        s.reasoning = u.opt_str("reasoning");
        s.opinionScore = u.num("opinionScore");
        s.personaId = persona_id;
        s.displayName = it->second.display_name;
        s.traits = parse_traits(it->second.traits);
        rows.push_back(std::move(s));
    }
    return aggregate_persona_consensus(rows);
}

} // namespace

http::Router consensus_persona_routes() {
    http::Router router;
    router.get("/admin/consensus/:gapId/personas", [](http::Request& c) {
        if (auto guard = middleware::require_admin(c)) return *guard;
        auto with_cache = [](http::Response r) {
            r.headers.set("Cache-Control", "private, no-store");
            return r;
        };
        const auto& config = config::get_config();
        const auto gap_id = c.param("gapId");
        try {
            core::CoreOptions opts;
            opts.sqlitePath = core::resolve_active_kg_path(config);
            auto core = core::create_core(opts);
            const auto session = core->raw().get(
                "SELECT id FROM sessions WHERE workspace_id = ? AND title = ? ORDER BY started_at DESC LIMIT 1",
                {config.workspace, "discussion-of-gap:" + gap_id});
            if (!session) return with_cache(http::Response::json(Json{{"gapId", gap_id}, {"personas", Json::array()}}));

            sql::OpenOptions o;
            o.readonly = true;
            o.create = false;
            o.fileMustExist = true;
            o.busy_timeout_ms = 2000;
            auto persona_db = sql::Database::open(config.personaEngine.dbPath, o);
            Json personas = Json::array();
            for (const auto& s : load_persona_consensus(core->raw(), *persona_db, session->str("id"))) personas.push_back(to_json(s));
            return with_cache(http::Response::json(Json{{"gapId", gap_id}, {"personas", personas}}));
        } catch (const std::exception&) {
            // Database errors can contain local paths and SQL details; keep them out of the response.
            return with_cache(http::Response::json(Json{{"error", "persona consensus unavailable"}}, 503));
        }
    });
    return router;
}

} // namespace di::api
