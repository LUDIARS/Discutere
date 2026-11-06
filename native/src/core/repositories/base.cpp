#include "core/repositories/base.hpp"

#include <stdexcept>

#include "base/crypto.hpp"
#include "base/time.hpp"
#include "core/events/event_types.hpp"

namespace di::core::repositories {

namespace {

namespace ET = events::EVENT_TYPES;

template <class T>
void put(Json& j, const char* key, const std::optional<T>& v) {
    if (v) j[key] = *v;
}

/** `{ ...input, id }`: 呼び出し側が id を渡していれば先頭 (その位置のまま)、無ければ末尾。 */
Json with_id(const Json& fields, const std::optional<std::string>& given, const std::string& id) {
    if (!given) {
        Json out = fields;
        out["id"] = id;
        return out;
    }
    Json out = Json::object();
    out["id"] = id;
    for (auto it = fields.begin(); it != fields.end(); ++it) out[it.key()] = it.value();
    return out;
}

std::string append(const CoreContext& ctx, std::string_view event_type, Json payload) {
    events::AppendEventInput in;
    in.eventType = std::string(event_type);
    in.payload = std::move(payload);
    return ctx.eventLog->append_event(in).id;
}

std::string new_id(const std::optional<std::string>& given) { return given ? *given : crypto::uuid_v4(); }

double now() { return static_cast<double>(time::now_ms()); }

} // namespace

CoreContext make_context(db::KuzuClient& client, events::EventLog& event_log) { return CoreContext{&client, &event_log}; }

std::optional<sql::Row> TableReader::get(const std::string& id) const {
    return ctx_.client->raw().get("SELECT * FROM " + table_ + " WHERE id = ?", {id});
}

std::vector<sql::Row> TableReader::list(const std::string& workspace_id) const {
    return ctx_.client->raw().all("SELECT * FROM " + table_ + " WHERE workspace_id = ? ORDER BY created_at DESC",
                                  {workspace_id}, kRepoListLimits);
}

// ─── person ──────────────────────────────────────────────────────
static Json person_fields(const std::string& ws, const std::string& name, const std::optional<std::string>& role) {
    Json j = Json::object();
    j["workspaceId"] = ws;
    j["name"] = name;
    put(j, "role", role);
    return j;
}
std::string PersonRepo::create(const PersonInput& input) const {
    const std::string id = new_id(input.id);
    append(ctx_, ET::PersonCreated, with_id(person_fields(input.workspaceId, input.name, input.role), input.id, id));
    return id;
}
void PersonRepo::update(const std::string& id, const PersonPatch& patch) const {
    append(ctx_, ET::PersonUpdated, with_id(person_fields(patch.workspaceId, patch.name, patch.role), std::nullopt, id));
}

// ─── game (索引表へ直接) ─────────────────────────────────────────────
std::string GameRepo::create(const GameInput& input) const {
    const std::string id = new_id(input.id);
    const double at = now();
    ctx_.client->raw().run("INSERT INTO games (id, workspace_id, title, genre, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?)",
                           {id, input.workspaceId, input.title, input.genre, at, at});
    Json payload = Json::object();
    payload["id"] = crypto::uuid_v4();
    payload["workspaceId"] = input.workspaceId;
    payload["gameId"] = id;
    append(ctx_, ET::PlayContextCaptured, std::move(payload));
    return id;
}
void GameRepo::update(const std::string& id, const GamePatch& patch) const {
    ctx_.client->raw().run("UPDATE games SET title = ?, genre = ?, updated_at = ? WHERE id = ?", {patch.title, patch.genre, now(), id});
}

// ─── mechanic ────────────────────────────────────────────────────
static Json mechanic_fields(const std::string& ws, const std::optional<std::string>& game_id, const std::string& name,
                            const std::optional<std::string>& description) {
    Json j = Json::object();
    j["workspaceId"] = ws;
    put(j, "gameId", game_id);
    j["name"] = name;
    put(j, "description", description);
    return j;
}
std::string MechanicRepo::create(const MechanicInput& input) const {
    const std::string id = new_id(input.id);
    append(ctx_, ET::MechanicProposed, with_id(mechanic_fields(input.workspaceId, input.gameId, input.name, input.description), input.id, id));
    return id;
}
void MechanicRepo::update(const std::string& id, const MechanicPatch& patch) const {
    append(ctx_, ET::MechanicProposed, with_id(mechanic_fields(patch.workspaceId, patch.gameId, patch.name, patch.description), std::nullopt, id));
}

// ─── aesthetic (索引表へ直接) ────────────────────────────────────────
std::string AestheticRepo::create(const AestheticInput& input) const {
    const std::string id = new_id(input.id);
    const double at = now();
    ctx_.client->raw().run(
        "INSERT INTO aesthetics (id, workspace_id, game_id, name, description, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?)",
        {id, input.workspaceId, input.gameId, input.name, input.description, at, at});
    return id;
}
void AestheticRepo::update(const std::string& id, const AestheticPatch& patch) const {
    ctx_.client->raw().run("UPDATE aesthetics SET name = ?, description = ?, updated_at = ? WHERE id = ?",
                           {patch.name, patch.description, now(), id});
}

// ─── affect ──────────────────────────────────────────────────────
static Json affect_fields(const std::string& ws, const std::optional<std::string>& session_id, const std::optional<std::string>& subject_id,
                          const std::string& mood, const std::optional<double>& score) {
    Json j = Json::object();
    j["workspaceId"] = ws;
    put(j, "sessionId", session_id);
    put(j, "subjectId", subject_id);
    j["mood"] = mood;
    put(j, "score", score);
    return j;
}
std::string AffectRepo::create(const AffectInput& input) const {
    const std::string id = new_id(input.id);
    append(ctx_, ET::AffectMeasured, with_id(affect_fields(input.workspaceId, input.sessionId, input.subjectId, input.mood, input.score), input.id, id));
    return id;
}
void AffectRepo::update(const std::string& id, const AffectPatch& patch) const {
    append(ctx_, ET::AffectMeasured, with_id(affect_fields(patch.workspaceId, patch.sessionId, patch.subjectId, patch.mood, patch.score), std::nullopt, id));
}

// ─── play context ────────────────────────────────────────────────
static Json play_context_fields(const std::string& ws, const std::optional<std::string>& game_id, const std::optional<std::string>& platform,
                                const std::optional<std::string>& mode) {
    Json j = Json::object();
    j["workspaceId"] = ws;
    put(j, "gameId", game_id);
    put(j, "platform", platform);
    put(j, "mode", mode);
    return j;
}
std::string PlayContextRepo::create(const PlayContextInput& input) const {
    const std::string id = new_id(input.id);
    append(ctx_, ET::PlayContextCaptured, with_id(play_context_fields(input.workspaceId, input.gameId, input.platform, input.mode), input.id, id));
    return id;
}
void PlayContextRepo::update(const std::string& id, const PlayContextPatch& patch) const {
    append(ctx_, ET::PlayContextCaptured, with_id(play_context_fields(patch.workspaceId, patch.gameId, patch.platform, patch.mode), std::nullopt, id));
}

// ─── session ─────────────────────────────────────────────────────
static Json session_fields(const std::string& ws, const std::string& title, double started_at, const std::optional<double>& ended_at,
                           const std::optional<std::string>& mode, const std::optional<std::string>& scene) {
    Json j = Json::object();
    j["workspaceId"] = ws;
    j["title"] = title;
    j["startedAt"] = started_at;
    put(j, "endedAt", ended_at);
    put(j, "mode", mode);
    put(j, "scene", scene);
    return j;
}
std::string SessionRepo::create(const SessionInput& input) const {
    const std::string id = new_id(input.id);
    append(ctx_, ET::SessionCreated,
           with_id(session_fields(input.workspaceId, input.title, input.startedAt, input.endedAt, input.mode, input.scene), input.id, id));
    return id;
}
void SessionRepo::update(const std::string& id, const SessionPatch& patch) const {
    append(ctx_, ET::SessionUpdated,
           with_id(session_fields(patch.workspaceId, patch.title, patch.startedAt, patch.endedAt, patch.mode, patch.scene), std::nullopt, id));
}

// ─── utterance ───────────────────────────────────────────────────
std::string UtteranceRepo::create(const UtteranceInput& input) const {
    const std::string id = new_id(input.id);
    Json j = Json::object();
    j["workspaceId"] = input.workspaceId;
    j["sessionId"] = input.sessionId;
    put(j, "speakerId", input.speakerId);
    j["rawContent"] = input.rawContent;
    put(j, "normalizedContent", input.normalizedContent);
    j["postedAt"] = input.postedAt;
    put(j, "respondsTo", input.respondsTo);
    append(ctx_, ET::UtteranceCreated, with_id(j, input.id, id));
    return id;
}
void UtteranceRepo::update(const std::string& id, const UtterancePatch& patch) const {
    const auto current = get(id);
    if (!current) throw std::runtime_error("utterance not found");
    // 未取得 (raw_content が NULL) の行と比べるときも TS の !== と同じく「違う」とみなす。
    if (patch.rawContent && (current->is_null("raw_content") || *patch.rawContent != current->str("raw_content")))
        throw std::runtime_error("Utterance.raw_content is immutable");
    Json j = Json::object();
    j["id"] = id;
    j["workspaceId"] = patch.workspaceId;
    j["normalizedContent"] = patch.normalizedContent;
    append(ctx_, ET::UtteranceNormalized, std::move(j));
}

// ─── reaction ────────────────────────────────────────────────────
static Json reaction_fields(const std::string& ws, const std::string& utterance_id, const std::optional<std::string>& actor_id,
                            const std::string& reaction_type, const std::optional<double>& intensity) {
    Json j = Json::object();
    j["workspaceId"] = ws;
    j["utteranceId"] = utterance_id;
    put(j, "actorId", actor_id);
    j["reactionType"] = reaction_type;
    put(j, "intensity", intensity);
    return j;
}
std::string ReactionRepo::create(const ReactionInput& input) const {
    const std::string id = new_id(input.id);
    append(ctx_, ET::ReactionAdded,
           with_id(reaction_fields(input.workspaceId, input.utteranceId, input.actorId, input.reactionType, input.intensity), input.id, id));
    return id;
}
void ReactionRepo::update(const std::string& id, const ReactionPatch& patch) const {
    append(ctx_, ET::ReactionAdded,
           with_id(reaction_fields(patch.workspaceId, patch.utteranceId, patch.actorId, patch.reactionType, patch.intensity), std::nullopt, id));
}

// ─── design gap ──────────────────────────────────────────────────
static Json design_gap_fields(const std::string& ws, const std::optional<std::string>& game_id, const std::string& title,
                              const std::optional<std::string>& description, const std::optional<std::string>& status) {
    Json j = Json::object();
    j["workspaceId"] = ws;
    put(j, "gameId", game_id);
    j["title"] = title;
    put(j, "description", description);
    put(j, "status", status);
    return j;
}
std::string DesignGapRepo::create(const DesignGapInput& input) const {
    const std::string id = new_id(input.id);
    append(ctx_, ET::DesignGapDetected,
           with_id(design_gap_fields(input.workspaceId, input.gameId, input.title, input.description, input.status), input.id, id));
    return id;
}
void DesignGapRepo::update(const std::string& id, const DesignGapPatch& patch) const {
    append(ctx_, ET::DesignGapUpdated,
           with_id(design_gap_fields(patch.workspaceId, patch.gameId, patch.title, patch.description, patch.status), std::nullopt, id));
}

// ─── hypothesis ──────────────────────────────────────────────────
static Json hypothesis_fields(const std::string& ws, const std::optional<std::string>& design_gap_id, const std::string& statement,
                              const std::optional<std::string>& status, const std::optional<bool>& integrated) {
    Json j = Json::object();
    j["workspaceId"] = ws;
    put(j, "designGapId", design_gap_id);
    j["statement"] = statement;
    put(j, "status", status);
    put(j, "integrated", integrated);
    return j;
}
std::string HypothesisRepo::create(const HypothesisInput& input) const {
    const std::string id = new_id(input.id);
    append(ctx_, ET::HypothesisCreated,
           with_id(hypothesis_fields(input.workspaceId, input.designGapId, input.statement, input.status, input.integrated), input.id, id));
    return id;
}
void HypothesisRepo::update(const std::string& id, const HypothesisPatch& patch) const {
    append(ctx_, ET::HypothesisUpdated,
           with_id(hypothesis_fields(patch.workspaceId, patch.designGapId, patch.statement, patch.status, patch.integrated), std::nullopt, id));
}

} // namespace di::core::repositories
