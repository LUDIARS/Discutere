#include "web_chat/session.hpp"

#include "base/strings.hpp"
#include "base/time.hpp"

namespace di::web_chat {

std::string web_scene(std::string_view room_id) { return "web:" + std::string(room_id); }

std::string sanitize_room_id(std::string_view raw) {
    // (raw ?? "").trim().replace(/[^A-Za-z0-9._-]/g, "") — 残るのは ASCII だけなので slice(0, 32) はバイト数で同じ。
    const auto trimmed = str::trim(raw);
    std::string v;
    for (const char c : trimmed) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (ok) v.push_back(c);
    }
    if (v.empty()) return "lobby";
    return v.substr(0, 32);
}

std::string ensure_web_session(core::Core& core, const std::string& workspace_id, const std::string& room_id) {
    const std::string title = "web-session:" + room_id;
    if (auto existing = core.raw().get("SELECT id FROM sessions WHERE workspace_id = ? AND title = ? ORDER BY started_at DESC LIMIT 1",
                                       {workspace_id, title}))
        return existing->str("id");
    core::repositories::SessionInput input;
    input.workspaceId = workspace_id;
    input.title = title;
    input.startedAt = static_cast<double>(time::now_ms());
    input.scene = web_scene(room_id);
    return core.repos.session.create(input);
}

bool web_discussion_exists(core::Core& core, const std::string& workspace_id, const std::string& room_id) {
    const auto row = core.raw().get(
        "SELECT s.id AS id\n"
        "   FROM sessions s\n"
        "   JOIN design_gaps g\n"
        "     ON g.id = SUBSTR(s.title, LENGTH('discussion-of-gap:') + 1)\n"
        "  WHERE s.workspace_id = ?\n"
        "    AND s.title LIKE 'discussion-of-gap:%'\n"
        "    AND s.scene = ?\n"
        "    AND (g.status IS NULL OR g.status NOT IN ('closed', 'converged', 'dismissed'))\n"
        "  LIMIT 1",
        {workspace_id, web_scene(room_id)});
    return row.has_value();
}

} // namespace di::web_chat
