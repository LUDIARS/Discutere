#include "flow/paper_revisions.hpp"

#include "base/time.hpp"
#include "flow/db/connection.hpp"

namespace di::flow {

namespace {

std::int64_t latest_rev(const std::string& session_id) {
    auto row = db::get_flow_db()->get("SELECT MAX(rev) AS maxRev FROM discussion_paper_revision WHERE session_id = ?",
                                      {session_id});
    return row ? row->opt_i64("maxRev").value_or(0) : 0;
}

PaperRevision row_to_revision(const sql::Row& r) {
    PaperRevision out;
    out.rev = r.i64("rev");
    out.bodyMd = r.str("body_md");
    out.changeSummary = r.str("change_summary");
    out.origin = r.str("origin");
    out.createdAt = r.i64("created_at");
    return out;
}

} // namespace

Json to_json(const PaperRevision& r) {
    Json o = Json::object();
    o["rev"] = r.rev;
    o["bodyMd"] = r.bodyMd;
    o["changeSummary"] = r.changeSummary;
    o["origin"] = r.origin;
    o["createdAt"] = r.createdAt;
    return o;
}

std::int64_t append_revision(const AppendRevisionArgs& args) {
    auto db = db::get_flow_db();
    const auto current = latest_revision(args.sessionId);
    if (current && current->bodyMd == args.bodyMd) return current->rev;
    const std::int64_t rev = latest_rev(args.sessionId) + 1;
    db->run(R"SQL(INSERT INTO discussion_paper_revision (session_id, rev, body_md, change_summary, origin, created_at)
     VALUES (?, ?, ?, ?, ?, ?))SQL",
            {args.sessionId, rev, args.bodyMd, args.changeSummary, args.origin, time::now_ms()});
    return rev;
}

std::optional<PaperRevision> latest_revision(const std::string& session_id) {
    auto row = db::get_flow_db()->get(
        R"SQL(SELECT rev, body_md, change_summary, origin, created_at
         FROM discussion_paper_revision
        WHERE session_id = ?
        ORDER BY rev DESC LIMIT 1)SQL",
        {session_id});
    if (!row) return std::nullopt;
    return row_to_revision(*row);
}

std::vector<PaperRevision> list_revisions(const std::string& session_id) {
    const auto rows = db::get_flow_db()->all(
        R"SQL(SELECT rev, body_md, change_summary, origin, created_at
         FROM discussion_paper_revision
        WHERE session_id = ?
        ORDER BY rev ASC)SQL",
        {session_id});
    std::vector<PaperRevision> out;
    out.reserve(rows.size());
    for (const auto& r : rows) out.push_back(row_to_revision(r));
    return out;
}

bool can_revert(const std::string& session_id) { return latest_rev(session_id) >= 2; }

std::optional<PaperRevision> revert_last(const std::string& session_id) {
    const auto all = list_revisions(session_id);
    if (all.size() < 2) return std::nullopt;
    const auto& previous = all[all.size() - 2];
    const auto rev = append_revision({session_id, previous.bodyMd,
                                      "1 手前 (rev " + std::to_string(previous.rev) + ") に戻しました", "revert"});
    if (auto latest = latest_revision(session_id)) return latest;
    PaperRevision fallback = previous;
    fallback.rev = rev;
    fallback.origin = "revert";
    return fallback;
}

} // namespace di::flow
