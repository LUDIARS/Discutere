#include "core/hypothesis/lifecycle_handler.hpp"

#include "core/events/event_types.hpp"
#include "core/projection/command_handlers/types.hpp"

namespace di::core::hypothesis {

namespace ch = projection::command_handlers;

TransitionResult apply_lifecycle(projection::ReturnTypeCreateCore& core, const std::string& hypothesis_id, std::string_view action,
                                 const LifecycleExtra& extra) {
    const auto h = core.raw().get("SELECT * FROM hypotheses WHERE id = ?", {hypothesis_id});
    if (!h) return {false, {}, "hypothesis not found"};
    // 現在の状態 (status が無ければ draft)。
    const std::string current = h->is_null("status") ? std::string("draft") : h->str("status");
    auto tr = can_transition(current, action);
    if (!tr.ok) return tr;

    Json payload = Json::object();
    payload["id"] = (*h)["id"].to_json();
    payload["workspaceId"] = !extra.workspaceId.empty() ? Json(extra.workspaceId) : (*h)["workspace_id"].to_json();
    payload["statement"] = (*h)["statement"].to_json();
    ch::put_if_not_null(payload, "designGapId", (*h)["design_gap_id"]);
    payload["status"] = tr.to;
    payload["integrated"] = tr.to == "integrated";
    payload["validatedByEmotion"] = tr.to == "validated_by_emotion" || tr.to == "integrated";
    if (extra.sessionId) payload["sessionId"] = *extra.sessionId;
    payload["refs"] = ch::parse_refs_json((*h)["refs_json"]);

    namespace E = events::EVENT_TYPES;
    const std::string_view ev = action == "submit"        ? E::HypothesisProposed
                                : action == "integrate"   ? E::HypothesisIntegrated
                                : action == "reject"      ? E::HypothesisRejected
                                : action == "session_end" ? E::HypothesisValidated
                                                          : E::HypothesisDebated;
    core.eventLog.append_event({std::nullopt, std::string(ev), payload, std::nullopt});
    return tr;
}

} // namespace di::core::hypothesis
