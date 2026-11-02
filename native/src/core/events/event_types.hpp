#pragma once
// ドメインイベントの種類と payload (src/core/events/event-types.ts)。
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.hpp"

namespace di::core::events {

namespace EVENT_TYPES {
inline constexpr std::string_view PersonCreated = "PersonCreated";
inline constexpr std::string_view PersonUpdated = "PersonUpdated";
inline constexpr std::string_view SessionCreated = "SessionCreated";
inline constexpr std::string_view SessionUpdated = "SessionUpdated";
inline constexpr std::string_view SessionEnded = "SessionEnded";
inline constexpr std::string_view UtteranceCreated = "UtteranceCreated";
inline constexpr std::string_view UtteranceNormalized = "UtteranceNormalized";
inline constexpr std::string_view ReactionAdded = "ReactionAdded";
inline constexpr std::string_view DesignGapDetected = "DesignGapDetected";
inline constexpr std::string_view DesignGapUpdated = "DesignGapUpdated";
inline constexpr std::string_view HypothesisCreated = "HypothesisCreated";
inline constexpr std::string_view HypothesisUpdated = "HypothesisUpdated";
inline constexpr std::string_view HypothesisProposed = "HypothesisProposed";
inline constexpr std::string_view HypothesisDebated = "HypothesisDebated";
inline constexpr std::string_view HypothesisValidated = "HypothesisValidated";
inline constexpr std::string_view HypothesisIntegrated = "HypothesisIntegrated";
inline constexpr std::string_view HypothesisRejected = "HypothesisRejected";
inline constexpr std::string_view HypothesisStaleFlagged = "HypothesisStaleFlagged";
inline constexpr std::string_view MechanicProposed = "MechanicProposed";
inline constexpr std::string_view MechanicRefined = "MechanicRefined";
inline constexpr std::string_view TranslationProposed = "TranslationProposed";
inline constexpr std::string_view TranslationApproved = "TranslationApproved";
inline constexpr std::string_view TranslationRevised = "TranslationRevised";
inline constexpr std::string_view TranslationRejected = "TranslationRejected";
inline constexpr std::string_view AffectAdded = "AffectAdded";
inline constexpr std::string_view AffectMeasured = "AffectMeasured";
inline constexpr std::string_view PlayContextCaptured = "PlayContextCaptured";
} // namespace EVENT_TYPES

/** EVENT_TYPES の全値 (定義順)。 */
const std::vector<std::string_view>& all_event_types();
bool is_event_type(std::string_view name);

// ─── payload (省略可能な項目は nullopt なら JSON に出さない) ─────────────
struct BasePayload {
    std::string workspaceId;
    std::string id;
    std::optional<double> at;
};

struct PersonPayload : BasePayload {
    std::string name;
    std::optional<std::string> role;
};
struct SessionPayload : BasePayload {
    std::string title;
    double startedAt = 0;
    std::optional<double> endedAt;
    std::optional<std::string> mode;
    std::optional<std::string> scene;
};
struct UtteranceCreatedPayload : BasePayload {
    std::string sessionId;
    std::optional<std::string> speakerId;
    std::string rawContent;
    std::optional<std::string> normalizedContent;
    double postedAt = 0;
    std::optional<std::string> respondsTo;
};
struct UtteranceNormalizedPayload : BasePayload {
    std::string normalizedContent;
};
struct ReactionPayload : BasePayload {
    std::string utteranceId;
    std::optional<std::string> actorId;
    std::string reactionType;
    std::optional<double> intensity;
};
struct DesignGapPayload : BasePayload {
    std::optional<std::string> gameId;
    std::string title;
    std::optional<std::string> description;
    std::optional<std::string> status;
};
struct HypothesisPayload : BasePayload {
    std::optional<std::string> designGapId;
    std::string statement;
    std::optional<std::string> status;
    std::optional<bool> integrated;
    std::optional<bool> validatedByEmotion;
    std::optional<bool> staleFlagged;
    std::optional<std::string> sessionId;
    std::optional<std::vector<std::string>> refs;
};
struct MechanicProposedPayload : BasePayload {
    std::optional<std::string> gameId;
    std::string name;
    std::optional<std::string> description;
    std::optional<std::string> intends;
};
struct AffectMeasuredPayload : BasePayload {
    std::optional<std::string> sessionId;
    std::optional<std::string> subjectId;
    std::string mood;
    std::optional<double> score;
};
struct PlayContextPayload : BasePayload {
    std::optional<std::string> gameId;
    std::optional<std::string> platform;
    std::optional<std::string> mode;
};

Json to_json(const PersonPayload& p);
Json to_json(const SessionPayload& p);
Json to_json(const UtteranceCreatedPayload& p);
Json to_json(const UtteranceNormalizedPayload& p);
Json to_json(const ReactionPayload& p);
Json to_json(const DesignGapPayload& p);
Json to_json(const HypothesisPayload& p);
Json to_json(const MechanicProposedPayload& p);
Json to_json(const AffectMeasuredPayload& p);
Json to_json(const PlayContextPayload& p);

/** DomainEvent。payload は種類ごとに形が違うので JSON のまま持つ (TS の (payload as any) と同じ読み方)。 */
struct DomainEvent {
    std::string id;
    std::string eventType;
    Json payload = Json::object();
    double createdAt = 0;
};

Json to_json(const DomainEvent& e);
/** 形が違う (id/eventType が文字列でない・payload が object でない 等) なら std::invalid_argument。 */
DomainEvent domain_event_from_json(const Json& j);

} // namespace di::core::events
