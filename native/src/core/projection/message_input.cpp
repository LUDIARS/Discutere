#include "core/projection/message_input.hpp"

#include <array>
#include <stdexcept>
#include <string_view>

#include "base/time.hpp"
#include "core/projection/command_handlers/cross/index.hpp"
#include "core/projection/command_handlers/emotion/index.hpp"
#include "core/projection/command_handlers/learning/index.hpp"
#include "core/projection/command_handlers/synthesis/index.hpp"
#include "core/projection/command_parser.hpp"
#include "core/projection/responds_to_inference.hpp"

namespace di::core::projection {

namespace ch = command_handlers;

Json to_json(const CommandResult& r) {
    Json o = Json::object();
    o["ok"] = r.ok;
    if (r.message) o["message"] = *r.message;
    if (r.error) o["error"] = *r.error;
    return o;
}

Json to_json(const SubmitMessageResult& r) {
    Json o = Json::object();
    o["utteranceId"] = r.utteranceId;
    if (r.commandResult) o["commandResult"] = to_json(*r.commandResult);
    return o;
}

namespace {

ch::HandlerResult dispatch(const std::string& c, const ch::HandlerContext& ctx) {
    if (c == "define") return ch::learning::handle_define(ctx);
    if (c == "intends") return ch::learning::handle_intends(ctx);
    if (c == "refine") return ch::learning::handle_refine(ctx);
    if (c == "compare") return ch::learning::handle_compare(ctx);
    if (c == "history") return ch::learning::handle_history(ctx);
    if (c == "scene") return ch::emotion::handle_scene(ctx);
    if (c == "done") return ch::emotion::handle_done(ctx);
    if (c == "refs") return ch::synthesis::handle_refs(ctx);
    if (c == "propose") return ch::synthesis::handle_propose(ctx);
    if (c == "addresses") return ch::synthesis::handle_addresses(ctx);
    if (c == "jump") return ch::synthesis::handle_jump(ctx);
    if (c == "submit") return ch::synthesis::handle_submit(ctx);
    if (c == "validate") return ch::synthesis::handle_validate(ctx);
    if (c == "integrate") return ch::synthesis::handle_integrate(ctx);
    if (c == "reject") return ch::synthesis::handle_reject(ctx);
    static constexpr std::array<std::string_view, 6> kCross{"me", "find", "lineage", "cluster", "stale", "hotspot"};
    for (const auto name : kCross)
        if (c == name) return ch::cross::handle_cross(ctx, c);
    return ch::HandlerResult::failure("unknown command: " + c);
}

} // namespace

SubmitMessageResult submit_message(const SubmitMessageInput& input) {
    if (!input.core) throw std::invalid_argument("submit_message: core is required");
    auto& core = *input.core;
    if (const auto parsed = parse_command(input.rawContent)) {
        ch::HandlerContext ctx;
        ctx.core = input.core;
        ctx.workspaceId = input.workspaceId;
        ctx.sessionId = input.sessionId;
        ctx.personId = input.personId;
        ctx.args = parsed->args;
        const auto r = dispatch(parsed->command, ctx);
        SubmitMessageResult out;
        CommandResult cr;
        cr.ok = r.ok;
        if (r.ok)
            cr.message = r.message;
        else
            cr.error = r.error;
        out.commandResult = std::move(cr);
        return out;
    }

    repositories::UtteranceInput u;
    u.workspaceId = input.workspaceId;
    u.sessionId = input.sessionId;
    u.speakerId = input.personId;
    u.rawContent = input.rawContent;
    u.postedAt = static_cast<double>(time::now_ms());
    u.respondsTo = infer_responds_to(input.rawContent);
    SubmitMessageResult out;
    out.utteranceId = core.repos.utterance.create(u);
    return out;
}

} // namespace di::core::projection
