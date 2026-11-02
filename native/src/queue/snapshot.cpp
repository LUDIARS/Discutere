#include "queue/snapshot.hpp"

#include "base/strings.hpp"
#include "base/time.hpp"

namespace di::queue {

namespace {

constexpr std::int64_t SESSION_LIMIT = 50;
constexpr std::int64_t GAP_LIMIT = 100;
constexpr std::int64_t HYPOTHESIS_LIMIT = 100;

struct SceneParts {
    std::optional<std::string> guildId;
    std::optional<std::string> channelId;
};

/** "discord:<guild>/<channel>" を分ける (空は null)。 */
SceneParts parse_scene(const std::optional<std::string>& scene) {
    if (!scene || !str::starts_with(*scene, "discord:")) return {};
    const auto parts = str::split(std::string_view(*scene).substr(8), "/");
    SceneParts out;
    if (!parts.empty() && !parts[0].empty()) out.guildId = parts[0];
    if (parts.size() > 1 && !parts[1].empty()) out.channelId = parts[1];
    return out;
}

/** s.length <= max ? s : s.slice(0, max - 1) + "…" (UTF-16 単位) */
std::string truncate(const std::string& s, std::int64_t max) {
    if (static_cast<std::int64_t>(str::js_length(s)) <= max) return s;
    return str::js_slice(s, 0, max - 1) + "…";
}

Json opt(const std::optional<std::string>& v) { return v ? Json(*v) : Json(nullptr); }

}  // namespace

QueueSnapshot build_queue_snapshot(sql::Database& core_db, sql::Database* pe_db, const std::string& workspace_id,
                                   std::optional<std::int64_t> now_ms) {
    QueueSnapshot snap;
    snap.workspaceId = workspace_id;
    const auto sessions = core_db.all(R"(SELECT id, title, scene, started_at
         FROM sessions
        WHERE workspace_id = ? AND scene LIKE 'discord:%' AND ended_at IS NULL
        ORDER BY started_at DESC
        LIMIT ?)",
                                      {workspace_id, SESSION_LIMIT});
    for (const auto& s : sessions) {
        const std::string id = s.str("id");
        auto u = core_db.get("SELECT COUNT(*) AS c, MAX(posted_at) AS last FROM utterances WHERE session_id = ?", {id});
        Json fire = 0;
        if (pe_db) {
            auto f = pe_db->get("SELECT COALESCE(SUM(count), 0) AS total FROM session_rule_fires WHERE session_id = ?", {id});
            if (f) fire = (*f)["total"].to_json();
        }
        QueueSessionEntry e;
        e.sessionId = id;
        e.title = s.str("title");
        e.scene = s.opt_str("scene");
        const auto parts = parse_scene(e.scene);
        e.guildId = parts.guildId;
        e.channelId = parts.channelId;
        e.startedAt = s["started_at"].to_json();
        e.utteranceCount = u ? u->i64("c") : 0;
        e.lastUtteranceAt = u ? (*u)["last"].to_json() : Json(nullptr);
        e.fireCount = fire;
        snap.activeSessions.push_back(std::move(e));
    }
    for (const auto& g : core_db.all(R"(SELECT id, title, status
           FROM design_gaps
          WHERE workspace_id = ?
            AND (status IS NULL OR status NOT IN ('resolved', 'closed', 'rejected', 'converged', 'dismissed'))
          ORDER BY created_at DESC
          LIMIT ?)",
                                     {workspace_id, GAP_LIMIT}))
        snap.openGaps.push_back({g.str("id"), g.str("title"), g.opt_str("status")});
    for (const auto& h : core_db.all(R"(SELECT id, statement, status, design_gap_id
           FROM hypotheses
          WHERE workspace_id = ?
            AND integrated = 0
            AND (status IS NULL OR status NOT IN ('rejected', 'integrated'))
          ORDER BY created_at DESC
          LIMIT ?)",
                                     {workspace_id, HYPOTHESIS_LIMIT}))
        snap.pendingHypotheses.push_back({h.str("id"), h.str("statement"), h.opt_str("status"), h.opt_str("design_gap_id")});
    snap.generatedAt = now_ms ? *now_ms : time::now_ms();
    return snap;
}

Json to_json(const QueueSnapshot& snap) {
    Json sessions = Json::array();
    for (const auto& s : snap.activeSessions) {
        Json o = Json::object();
        o["sessionId"] = s.sessionId;
        o["title"] = s.title;
        o["scene"] = opt(s.scene);
        o["guildId"] = opt(s.guildId);
        o["channelId"] = opt(s.channelId);
        o["startedAt"] = s.startedAt;
        o["utteranceCount"] = s.utteranceCount;
        o["lastUtteranceAt"] = s.lastUtteranceAt;
        o["fireCount"] = s.fireCount;
        sessions.push_back(std::move(o));
    }
    Json gaps = Json::array();
    for (const auto& g : snap.openGaps) {
        Json o = Json::object();
        o["id"] = g.id;
        o["title"] = g.title;
        o["status"] = opt(g.status);
        gaps.push_back(std::move(o));
    }
    Json hyps = Json::array();
    for (const auto& h : snap.pendingHypotheses) {
        Json o = Json::object();
        o["id"] = h.id;
        o["statement"] = h.statement;
        o["status"] = opt(h.status);
        o["designGapId"] = opt(h.designGapId);
        hyps.push_back(std::move(o));
    }
    Json out = Json::object();
    out["workspaceId"] = snap.workspaceId;
    out["generatedAt"] = snap.generatedAt;
    out["activeSessions"] = std::move(sessions);
    out["openGaps"] = std::move(gaps);
    out["pendingHypotheses"] = std::move(hyps);
    Json totals = Json::object();
    totals["activeSessions"] = snap.activeSessions.size();
    totals["openGaps"] = snap.openGaps.size();
    totals["pendingHypotheses"] = snap.pendingHypotheses.size();
    out["totals"] = std::move(totals);
    return out;
}

std::string format_queue_text(const QueueSnapshot& snap) {
    std::vector<std::string> lines;
    lines.push_back("🧭 **議論キュー** (ws=`" + snap.workspaceId + "`) — session " + std::to_string(snap.activeSessions.size()) +
                    " / gap " + std::to_string(snap.openGaps.size()) + " / 仮説 " + std::to_string(snap.pendingHypotheses.size()));
    if (!snap.activeSessions.empty()) {
        lines.push_back("");
        lines.push_back("**進行中の議論**");
        for (std::size_t i = 0; i < snap.activeSessions.size() && i < 8; ++i) {
            const auto& s = snap.activeSessions[i];
            lines.push_back("• `" + s.channelId.value_or("?") + "` — 発話 " + std::to_string(s.utteranceCount) + " / 発火 " +
                            (s.fireCount.is_null() ? std::string("null") : json_stringify(s.fireCount)));
        }
    }
    if (!snap.openGaps.empty()) {
        lines.push_back("");
        lines.push_back("**未処理の DesignGap (仮説待ち)**");
        for (std::size_t i = 0; i < snap.openGaps.size() && i < 6; ++i) lines.push_back("• " + truncate(snap.openGaps[i].title, 80));
    }
    if (!snap.pendingHypotheses.empty()) {
        lines.push_back("");
        lines.push_back("**検証待ちの仮説**");
        for (std::size_t i = 0; i < snap.pendingHypotheses.size() && i < 6; ++i) {
            const auto& h = snap.pendingHypotheses[i];
            lines.push_back("• " + truncate(h.statement, 80) + " (" + h.status.value_or("open") + ")");
        }
    }
    if (snap.activeSessions.empty() && snap.openGaps.empty() && snap.pendingHypotheses.empty()) {
        lines.push_back("");
        lines.push_back("_キューは空です_");
    }
    return truncate(str::join(lines, "\n"), 1900);
}

} // namespace di::queue
