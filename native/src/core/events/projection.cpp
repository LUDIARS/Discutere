#include "core/events/projection.hpp"

#include "core/db/js_bind.hpp"

namespace di::core::events {
namespace {

using db::js_bind;

const Json& f(const DomainEvent& e, const char* key) { return db::js_field(e.payload, key); }

/** x ?? fallback */
sql::Value or_value(const Json& v, sql::Value fallback) { return v.is_null() ? fallback : js_bind(v); }

/** event.payload.at ?? event.createdAt */
sql::Value now_ms(const DomainEvent& e) { return or_value(f(e, "at"), sql::Value(e.createdAt)); }

} // namespace

void apply_event_projection(sql::Database& db, const DomainEvent& event) {
    namespace T = EVENT_TYPES;
    const sql::Value t = now_ms(event);
    const std::string_view type = event.eventType;
    const sql::Value id = js_bind(f(event, "id"));
    const sql::Value ws = js_bind(f(event, "workspaceId"));

    if (type == T::PersonCreated || type == T::PersonUpdated) {
        db.run(R"(INSERT INTO people (id, workspace_id, name, role, created_at, updated_at)
        VALUES (?, ?, ?, ?, ?, ?)
        ON CONFLICT(id) DO UPDATE SET name = excluded.name, role = excluded.role, updated_at = excluded.updated_at)",
               {id, ws, js_bind(f(event, "name")), or_value(f(event, "role"), {}), t, t});
        return;
    }
    if (type == T::SessionCreated || type == T::SessionUpdated || type == T::SessionEnded) {
        db.run(R"(INSERT INTO sessions (id, workspace_id, title, started_at, ended_at, mode, scene, created_at, updated_at)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(id) DO UPDATE SET title=excluded.title, started_at=excluded.started_at, ended_at=excluded.ended_at, mode=excluded.mode, scene=excluded.scene, updated_at=excluded.updated_at)",
               {id, ws, or_value(f(event, "title"), sql::Value("session")), or_value(f(event, "startedAt"), t),
                or_value(f(event, "endedAt"), {}), or_value(f(event, "mode"), {}), or_value(f(event, "scene"), {}), t, t});
        return;
    }
    if (type == T::UtteranceCreated) {
        db.run(R"(INSERT INTO utterances (id, workspace_id, session_id, speaker_id, raw_content, normalized_content, posted_at, responds_to, created_at, updated_at)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?))",
               {id, ws, js_bind(f(event, "sessionId")), or_value(f(event, "speakerId"), {}), js_bind(f(event, "rawContent")),
                or_value(f(event, "normalizedContent"), {}), js_bind(f(event, "postedAt")),
                or_value(f(event, "respondsTo"), {}), t, t});
        return;
    }
    if (type == T::UtteranceNormalized) {
        db.run("UPDATE utterances SET normalized_content = ?, updated_at = ? WHERE id = ?",
               {or_value(f(event, "normalizedContent"), {}), t, id});
        return;
    }
    if (type == T::ReactionAdded) {
        db.run(R"(INSERT INTO reactions (id, workspace_id, utterance_id, actor_id, reaction_type, intensity, created_at, updated_at)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(id) DO UPDATE SET reaction_type=excluded.reaction_type, intensity=excluded.intensity, updated_at=excluded.updated_at)",
               {id, ws, js_bind(f(event, "utteranceId")), or_value(f(event, "actorId"), {}), js_bind(f(event, "reactionType")),
                or_value(f(event, "intensity"), {}), t, t});
        return;
    }
    if (type == T::MechanicProposed || type == T::MechanicRefined) {
        db.run(R"(INSERT INTO mechanics (id, workspace_id, game_id, name, description, intends, created_at, updated_at)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(id) DO UPDATE SET game_id=excluded.game_id, name=excluded.name, description=excluded.description, intends=excluded.intends, updated_at=excluded.updated_at)",
               {id, ws, or_value(f(event, "gameId"), {}), js_bind(f(event, "name")), or_value(f(event, "description"), {}),
                or_value(f(event, "intends"), {}), t, t});
        return;
    }
    if (type == T::AffectMeasured) {
        db.run(R"(INSERT INTO affects (id, workspace_id, session_id, subject_id, mood, score, created_at, updated_at)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(id) DO UPDATE SET mood=excluded.mood, score=excluded.score, updated_at=excluded.updated_at)",
               {id, ws, or_value(f(event, "sessionId"), {}), or_value(f(event, "subjectId"), {}), js_bind(f(event, "mood")),
                or_value(f(event, "score"), {}), t, t});
        return;
    }
    if (type == T::PlayContextCaptured) {
        db.run(R"(INSERT INTO play_contexts (id, workspace_id, game_id, platform, mode, created_at, updated_at)
        VALUES (?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(id) DO UPDATE SET game_id=excluded.game_id, platform=excluded.platform, mode=excluded.mode, updated_at=excluded.updated_at)",
               {id, ws, or_value(f(event, "gameId"), {}), or_value(f(event, "platform"), {}), or_value(f(event, "mode"), {}), t, t});
        return;
    }
    if (type == T::DesignGapDetected || type == T::DesignGapUpdated) {
        db.run(R"(INSERT INTO design_gaps (id, workspace_id, game_id, title, description, status, created_at, updated_at)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(id) DO UPDATE SET game_id=excluded.game_id, title=excluded.title, description=excluded.description, status=excluded.status, updated_at=excluded.updated_at)",
               {id, ws, or_value(f(event, "gameId"), {}), js_bind(f(event, "title")), or_value(f(event, "description"), {}),
                or_value(f(event, "status"), {}), t, t});
        return;
    }
    if (type == T::HypothesisCreated || type == T::HypothesisUpdated || type == T::HypothesisProposed ||
        type == T::HypothesisDebated || type == T::HypothesisValidated || type == T::HypothesisIntegrated ||
        type == T::HypothesisRejected || type == T::HypothesisStaleFlagged) {
        const Json& refs = f(event, "refs");
        db.run(R"(INSERT INTO hypotheses (id, workspace_id, design_gap_id, statement, status, integrated, validated_by_emotion, refs_json, created_at, updated_at)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(id) DO UPDATE SET design_gap_id=excluded.design_gap_id, statement=excluded.statement, status=excluded.status, integrated=excluded.integrated, validated_by_emotion=excluded.validated_by_emotion, refs_json=excluded.refs_json, updated_at=excluded.updated_at)",
               {id, ws, or_value(f(event, "designGapId"), {}), js_bind(f(event, "statement")), or_value(f(event, "status"), {}),
                json_truthy(f(event, "integrated")) ? 1.0 : 0.0, json_truthy(f(event, "validatedByEmotion")) ? 1.0 : 0.0,
                json_stringify(refs.is_null() ? Json::array() : refs), t, t});
        // staleFlagged !== undefined (null も「指定あり」)
        if (event.payload.is_object() && event.payload.contains("staleFlagged")) {
            db.run("UPDATE hypotheses SET stale_flagged = ?, updated_at = ? WHERE id = ?",
                   {json_truthy(f(event, "staleFlagged")) ? 1.0 : 0.0, t, id});
        }
        if (type == T::HypothesisDebated) {
            db.run("INSERT INTO hypothesis_validations (id, workspace_id, session_id, hypothesis_id, mode, created_at) VALUES (?, ?, ?, ?, ?, ?)",
                   {event.id, ws, or_value(f(event, "sessionId"), sql::Value("")), id,
                    or_value(f(event, "status"), sql::Value("unknown")), t});
        }
        return;
    }
    // 他のイベントは射影しない
}

} // namespace di::core::events
