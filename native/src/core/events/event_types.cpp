#include "core/events/event_types.hpp"

#include <stdexcept>

namespace di::core::events {
namespace {

template <class T>
void put(Json& j, const char* key, const std::optional<T>& v) {
    if (v) j[key] = *v;
}

Json base(const BasePayload& p) {
    Json j = Json::object();
    j["workspaceId"] = p.workspaceId;
    j["id"] = p.id;
    put(j, "at", p.at);
    return j;
}

} // namespace

const std::vector<std::string_view>& all_event_types() {
    using namespace EVENT_TYPES;
    static const std::vector<std::string_view> all = {
        PersonCreated,         PersonUpdated,        SessionCreated,       SessionUpdated,     SessionEnded,
        UtteranceCreated,      UtteranceNormalized,  ReactionAdded,        DesignGapDetected,  DesignGapUpdated,
        HypothesisCreated,     HypothesisUpdated,    HypothesisProposed,   HypothesisDebated,  HypothesisValidated,
        HypothesisIntegrated,  HypothesisRejected,   HypothesisStaleFlagged, MechanicProposed, MechanicRefined,
        TranslationProposed,   TranslationApproved,  TranslationRevised,   TranslationRejected, AffectAdded,
        AffectMeasured,        PlayContextCaptured};
    return all;
}

bool is_event_type(std::string_view name) {
    for (auto t : all_event_types())
        if (t == name) return true;
    return false;
}

Json to_json(const PersonPayload& p) {
    Json j = base(p);
    j["name"] = p.name;
    put(j, "role", p.role);
    return j;
}
Json to_json(const SessionPayload& p) {
    Json j = base(p);
    j["title"] = p.title;
    j["startedAt"] = p.startedAt;
    put(j, "endedAt", p.endedAt);
    put(j, "mode", p.mode);
    put(j, "scene", p.scene);
    return j;
}
Json to_json(const UtteranceCreatedPayload& p) {
    Json j = base(p);
    j["sessionId"] = p.sessionId;
    put(j, "speakerId", p.speakerId);
    j["rawContent"] = p.rawContent;
    put(j, "normalizedContent", p.normalizedContent);
    j["postedAt"] = p.postedAt;
    put(j, "respondsTo", p.respondsTo);
    return j;
}
Json to_json(const UtteranceNormalizedPayload& p) {
    Json j = base(p);
    j["normalizedContent"] = p.normalizedContent;
    return j;
}
Json to_json(const ReactionPayload& p) {
    Json j = base(p);
    j["utteranceId"] = p.utteranceId;
    put(j, "actorId", p.actorId);
    j["reactionType"] = p.reactionType;
    put(j, "intensity", p.intensity);
    return j;
}
Json to_json(const DesignGapPayload& p) {
    Json j = base(p);
    put(j, "gameId", p.gameId);
    j["title"] = p.title;
    put(j, "description", p.description);
    put(j, "status", p.status);
    return j;
}
Json to_json(const HypothesisPayload& p) {
    Json j = base(p);
    put(j, "designGapId", p.designGapId);
    j["statement"] = p.statement;
    put(j, "status", p.status);
    put(j, "integrated", p.integrated);
    put(j, "validatedByEmotion", p.validatedByEmotion);
    put(j, "staleFlagged", p.staleFlagged);
    put(j, "sessionId", p.sessionId);
    put(j, "refs", p.refs);
    return j;
}
Json to_json(const MechanicProposedPayload& p) {
    Json j = base(p);
    put(j, "gameId", p.gameId);
    j["name"] = p.name;
    put(j, "description", p.description);
    put(j, "intends", p.intends);
    return j;
}
Json to_json(const AffectMeasuredPayload& p) {
    Json j = base(p);
    put(j, "sessionId", p.sessionId);
    put(j, "subjectId", p.subjectId);
    j["mood"] = p.mood;
    put(j, "score", p.score);
    return j;
}
Json to_json(const PlayContextPayload& p) {
    Json j = base(p);
    put(j, "gameId", p.gameId);
    put(j, "platform", p.platform);
    put(j, "mode", p.mode);
    return j;
}

Json to_json(const DomainEvent& e) {
    return Json{{"id", e.id}, {"eventType", e.eventType}, {"payload", e.payload}, {"createdAt", e.createdAt}};
}

DomainEvent domain_event_from_json(const Json& j) {
    if (!j.is_object()) throw std::invalid_argument("DomainEvent: not an object");
    DomainEvent e;
    if (!j.contains("id") || !j["id"].is_string()) throw std::invalid_argument("DomainEvent: id must be a string");
    if (!j.contains("eventType") || !j["eventType"].is_string())
        throw std::invalid_argument("DomainEvent: eventType must be a string");
    if (!j.contains("payload") || !j["payload"].is_object())
        throw std::invalid_argument("DomainEvent: payload must be an object");
    e.id = j["id"].get<std::string>();
    e.eventType = j["eventType"].get<std::string>();
    e.payload = j["payload"];
    e.createdAt = json_number(j, "createdAt", 0);
    return e;
}

} // namespace di::core::events
