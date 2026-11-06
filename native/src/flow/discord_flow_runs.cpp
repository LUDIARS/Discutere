#include "flow/discord_flow_runs.hpp"

#include "base/crypto.hpp"
#include "flow/db/connection.hpp"
#include "flow/discussion_paper.hpp"
#include "flow/paper_markdown.hpp"
#include "flow/paper_revisions.hpp"

namespace di::flow {

std::string forum_session_id(const std::string& thread_id) {
    auto row = db::get_flow_db()->get("SELECT session_id FROM discord_flow_run WHERE thread_id = ?", {thread_id});
    return row ? row->str("session_id", thread_id) : thread_id;
}

std::optional<std::string> open_rediscussion(const std::string& thread_id) {
    auto db = db::get_flow_db();
    return db->transaction([&]() -> std::optional<std::string> {
        const std::string previous = forum_session_id(thread_id);
        auto paper = get_paper_snapshot(previous);
        if (!paper || (paper->flow != "discussion" && paper->flow != "improvement")) return std::nullopt;
        if (paper->status == "draft") return previous;
        const std::string session_id = "rediscussion-" + crypto::uuid_v4();
        const std::string body_md = strip_progress(paper->bodyMd);
        persist_draft_paper({session_id, paper->theme, paper->tags, paper->mechanics, paper->supplement, body_md},
                            paper->flow, std::string("discord"));
        append_revision({session_id, body_md, "前回のペーパーから再議論の草案を作成", "initial"});
        db->run(
            "INSERT INTO discord_flow_run (thread_id, session_id, previous_session_id) VALUES (?, ?, ?) ON CONFLICT(thread_id) DO UPDATE SET session_id=excluded.session_id, previous_session_id=excluded.previous_session_id",
            {thread_id, session_id, previous});
        return session_id;
    });
}

} // namespace di::flow
