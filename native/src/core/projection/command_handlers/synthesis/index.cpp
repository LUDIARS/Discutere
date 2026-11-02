#include "core/projection/command_handlers/synthesis/index.hpp"

#include "base/crypto.hpp"
#include "base/strings.hpp"
#include "core/events/event_types.hpp"
#include "core/hypothesis/lifecycle_handler.hpp"

namespace di::core::projection::command_handlers::synthesis {

namespace {
std::optional<sql::Row> latest_hypothesis(const HandlerContext& ctx) {
    return ctx.core->raw().get("SELECT * FROM hypotheses WHERE workspace_id = ? ORDER BY updated_at DESC LIMIT 1", {ctx.workspaceId});
}

hypothesis::LifecycleExtra extra_of(const HandlerContext& ctx) { return {ctx.workspaceId, ctx.sessionId}; }

HandlerResult apply(const HandlerContext& ctx, std::string_view action, std::string ok_message) {
    const auto h = latest_hypothesis(ctx);
    if (!h) return HandlerResult::failure("no hypothesis");
    const auto out = hypothesis::apply_lifecycle(*ctx.core, h->str("id"), action, extra_of(ctx));
    if (!out.ok) return HandlerResult::failure(out.error);
    return HandlerResult::success(std::move(ok_message));
}
} // namespace

HandlerResult handle_refs(const HandlerContext& ctx) {
    Json refs = Json::array();
    for (const auto& part : str::split(str::join(ctx.args, " "), ",")) {
        auto t = str::trim(part);
        if (!t.empty()) refs.push_back(std::move(t));
    }
    const auto h = latest_hypothesis(ctx);
    if (!h) return HandlerResult::failure("no hypothesis");
    Json payload = Json::object();
    payload["id"] = (*h)["id"].to_json();
    payload["workspaceId"] = ctx.workspaceId;
    payload["statement"] = (*h)["statement"].to_json();
    put_if_not_null(payload, "designGapId", (*h)["design_gap_id"]);
    put_if_not_null(payload, "status", (*h)["status"]);
    payload["integrated"] = js_truthy((*h)["integrated"]);
    payload["validatedByEmotion"] = js_truthy((*h)["validated_by_emotion"]);
    payload["refs"] = refs;
    ctx.core->eventLog.append_event({std::nullopt, std::string(events::EVENT_TYPES::HypothesisUpdated), payload, std::nullopt});
    return HandlerResult::success("refs linked");
}

HandlerResult handle_propose(const HandlerContext& ctx) {
    const std::string title = str::trim(str::join(ctx.args, " "));
    if (title.empty()) return HandlerResult::failure("usage: /propose <title>");
    const std::string id = crypto::uuid_v4();
    Json payload = Json::object();
    payload["id"] = id;
    payload["workspaceId"] = ctx.workspaceId;
    payload["statement"] = title;
    payload["status"] = "draft";
    payload["integrated"] = false;
    payload["validatedByEmotion"] = false;
    payload["refs"] = Json::array();
    ctx.core->eventLog.append_event({std::nullopt, std::string(events::EVENT_TYPES::HypothesisCreated), payload, std::nullopt});
    return HandlerResult::success("hypothesis " + id);
}

HandlerResult handle_addresses(const HandlerContext& ctx) {
    if (ctx.args.empty() || ctx.args[0].empty()) return HandlerResult::failure("usage: /addresses <gap_id>");
    const std::string& gap_id = ctx.args[0];
    const auto h = latest_hypothesis(ctx);
    if (!h) return HandlerResult::failure("no hypothesis");
    Json payload = Json::object();
    payload["id"] = (*h)["id"].to_json();
    payload["workspaceId"] = ctx.workspaceId;
    payload["statement"] = (*h)["statement"].to_json();
    payload["designGapId"] = gap_id;
    payload["status"] = h->is_null("status") ? Json("draft") : (*h)["status"].to_json();
    payload["integrated"] = js_truthy((*h)["integrated"]);
    payload["validatedByEmotion"] = js_truthy((*h)["validated_by_emotion"]);
    payload["refs"] = parse_refs_json((*h)["refs_json"]);
    ctx.core->eventLog.append_event({std::nullopt, std::string(events::EVENT_TYPES::HypothesisUpdated), payload, std::nullopt});
    return HandlerResult::success("addresses linked");
}

HandlerResult handle_jump(const HandlerContext& /*ctx*/) { return HandlerResult::success("jumped to axis1"); }

HandlerResult handle_submit(const HandlerContext& ctx) {
    const auto h = latest_hypothesis(ctx);
    if (!h) return HandlerResult::failure("no hypothesis");
    if (!js_truthy((*h)["design_gap_id"])) return HandlerResult::failure("/addresses is required before /submit");
    const auto out = hypothesis::apply_lifecycle(*ctx.core, h->str("id"), "submit", extra_of(ctx));
    if (!out.ok) return HandlerResult::failure(out.error);
    return HandlerResult::success("submitted");
}

HandlerResult handle_validate(const HandlerContext& ctx) {
    const std::string mode = str::to_lower(ctx.args.empty() ? std::string() : ctx.args[0]);
    if (mode != "theory" && mode != "emotion") return HandlerResult::failure("usage: /validate theory|emotion");
    const auto h = latest_hypothesis(ctx);
    if (!h) return HandlerResult::failure("no hypothesis");
    const std::string action = mode == "theory" ? "validate_theory" : "validate_emotion";
    const auto out = hypothesis::apply_lifecycle(*ctx.core, h->str("id"), action, extra_of(ctx));
    if (!out.ok) return HandlerResult::failure(out.error);
    // 2-phase commit を 1 コマンドで完了させる: under_X_validation → validated_by_X
    // (finalizeValidationForSession を session 終了まで待たない議論モード向け簡略化)
    const auto finalize = hypothesis::apply_lifecycle(*ctx.core, h->str("id"), "session_end", extra_of(ctx));
    if (!finalize.ok) return HandlerResult::success("validated " + mode + " (pending finalize: " + finalize.error + ")");
    return HandlerResult::success("validated " + mode);
}

HandlerResult handle_integrate(const HandlerContext& ctx) { return apply(ctx, "integrate", "integrated"); }

HandlerResult handle_reject(const HandlerContext& ctx) { return apply(ctx, "reject", "rejected"); }

} // namespace di::core::projection::command_handlers::synthesis
