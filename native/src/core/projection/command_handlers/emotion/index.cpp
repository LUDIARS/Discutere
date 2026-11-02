#include "core/projection/command_handlers/emotion/index.hpp"

#include "base/strings.hpp"
#include "base/time.hpp"
#include "core/events/event_types.hpp"
#include "core/hypothesis/validation_routing.hpp"

namespace di::core::projection::command_handlers::emotion {

namespace {
Json title_or_session(const std::optional<sql::Row>& s) {
    return (s && !s->is_null("title")) ? (*s)["title"].to_json() : Json("session");
}
Json started_or_now(const std::optional<sql::Row>& s) {
    return (s && !s->is_null("started_at")) ? (*s)["started_at"].to_json() : Json(time::now_ms());
}
} // namespace

HandlerResult handle_scene(const HandlerContext& ctx) {
    const std::string scene = str::trim(str::join(ctx.args, " "));
    if (scene.empty()) return HandlerResult::failure("usage: /scene <description>");
    const auto s = ctx.core->repos.session.get(ctx.sessionId);
    Json payload = Json::object();
    payload["id"] = ctx.sessionId;
    payload["workspaceId"] = ctx.workspaceId;
    payload["title"] = title_or_session(s);
    payload["startedAt"] = started_or_now(s);
    if (s) put_if_not_null(payload, "endedAt", (*s)["ended_at"]);
    payload["mode"] = "emotion";
    payload["scene"] = scene;
    ctx.core->eventLog.append_event({std::nullopt, std::string(events::EVENT_TYPES::SessionUpdated), payload, std::nullopt});
    return HandlerResult::success("scene set");
}

HandlerResult handle_done(const HandlerContext& ctx) {
    const auto s = ctx.core->repos.session.get(ctx.sessionId);
    Json payload = Json::object();
    payload["id"] = ctx.sessionId;
    payload["workspaceId"] = ctx.workspaceId;
    payload["title"] = title_or_session(s);
    payload["startedAt"] = started_or_now(s);
    payload["endedAt"] = time::now_ms();
    payload["mode"] = (s && !s->is_null("mode")) ? (*s)["mode"].to_json() : Json("emotion");
    if (s) put_if_not_null(payload, "scene", (*s)["scene"]);
    ctx.core->eventLog.append_event({std::nullopt, std::string(events::EVENT_TYPES::SessionEnded), payload, std::nullopt});
    hypothesis::finalize_validation_for_session(*ctx.core, ctx.workspaceId, ctx.sessionId);
    return HandlerResult::success("session ended");
}

} // namespace di::core::projection::command_handlers::emotion
