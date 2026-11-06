#include "core/projection/command_handlers/cross/find.hpp"

#include "base/strings.hpp"

namespace di::core::projection::command_handlers::cross {

namespace {
constexpr sql::QueryLimits kFindRowLimits{500'000, 256u << 20};
} // namespace

HandlerResult handle_find(const HandlerContext& ctx) {
    const std::string q = str::to_lower(str::trim(str::join(ctx.args, " ")));
    if (q.empty()) return HandlerResult::failure("usage: /find <query>");

    auto& raw = ctx.core->raw();
    std::vector<std::string> out;
    const auto mech = raw.all("SELECT id,name,description FROM mechanics WHERE workspace_id = ?", {ctx.workspaceId}, kFindRowLimits);
    const auto aff = raw.all("SELECT id,mood FROM affects WHERE workspace_id = ?", {ctx.workspaceId}, kFindRowLimits);
    const auto hyp = raw.all("SELECT id,statement,status FROM hypotheses WHERE workspace_id = ?", {ctx.workspaceId}, kFindRowLimits);
    const auto utt = raw.all("SELECT id,raw_content FROM utterances WHERE workspace_id = ?", {ctx.workspaceId}, kFindRowLimits);

    for (const auto& m : mech) {
        const std::string desc = m.is_null("description") ? std::string() : js_template(m["description"]);
        if (str::contains(str::to_lower(js_template(m["name"]) + " " + desc), q))
            out.push_back("mechanic:" + js_template(m["id"]) + ":" + js_template(m["name"]));
    }
    for (const auto& a : aff)
        if (str::contains(str::to_lower(js_template(a["mood"])), q)) out.push_back("affect:" + js_template(a["id"]) + ":" + js_template(a["mood"]));
    for (const auto& h : hyp)
        if (str::contains(str::to_lower(js_template(h["statement"])), q))
            out.push_back("hypothesis:" + js_template(h["id"]) + ":" + js_template(h["statement"]));
    for (const auto& u : utt)
        if (str::contains(str::to_lower(js_template(u["raw_content"])), q)) out.push_back("utterance:" + js_template(u["id"]));

    if (out.size() > 20) out.resize(20);
    const std::string message = str::join(out, "\n");
    return HandlerResult::success(message.empty() ? std::string("no results") : message);
}

} // namespace di::core::projection::command_handlers::cross
