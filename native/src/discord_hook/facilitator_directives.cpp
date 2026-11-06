#include "discord_hook/facilitator_directives.hpp"

#include <algorithm>

#include "base/crypto.hpp"
#include "base/strings.hpp"
#include "base/time.hpp"

namespace di::discord_hook {

void ensure_facilitator_directive_table(sql::Database& db) {
    db.exec(
        "CREATE TABLE IF NOT EXISTS facilitator_directives (\n"
        "       id TEXT PRIMARY KEY,\n"
        "       workspace_id TEXT NOT NULL,\n"
        "       gap_id TEXT NOT NULL,\n"
        "       text TEXT NOT NULL,\n"
        "       author_id TEXT,\n"
        "       created_at INTEGER NOT NULL\n"
        "     );\n"
        "     CREATE INDEX IF NOT EXISTS idx_facilitator_directives_gap\n"
        "       ON facilitator_directives (workspace_id, gap_id, created_at);");
}

std::optional<std::string> add_facilitator_directive(sql::Database& db, const FacilitatorDirectiveInput& input) {
    auto text = normalize_directive_text(input.text);
    if (!text) return std::nullopt;
    ensure_facilitator_directive_table(db);
    const std::string id = crypto::uuid_v4();
    db.run("INSERT INTO facilitator_directives (id, workspace_id, gap_id, text, author_id, created_at)\n     VALUES (?, ?, ?, ?, ?, ?)",
           {id, input.workspaceId, input.gapId, *text, sql::Value(input.authorId), time::now_ms()});
    return id;
}

std::vector<std::string> list_facilitator_directives(sql::Database& db, const std::string& workspace_id, const std::string& gap_id, int limit) {
    ensure_facilitator_directive_table(db);
    sql::QueryLimits q;
    q.max_rows = 1000;
    q.max_bytes = 1u << 20;
    // 同じ created_at (同一 ms) の行は挿入順 (rowid) で並べる (TS は SQLite の任意順だった)。
    auto rows = db.all(
        "SELECT text FROM facilitator_directives\n        WHERE workspace_id = ? AND gap_id = ?\n        ORDER BY created_at DESC, rowid DESC LIMIT ?",
        {workspace_id, gap_id, std::max(1, std::min(limit, 1000))}, q);
    std::vector<std::string> out;
    for (auto it = rows.rbegin(); it != rows.rend(); ++it) out.push_back(it->str("text"));
    return out;
}

std::optional<std::string> resolve_active_gap_for_scene(sql::Database& db, const std::string& workspace_id, const std::string& scene) {
    auto row = db.get(
        "SELECT SUBSTR(s.title, LENGTH('discussion-of-gap:') + 1) AS gap_id\n"
        "         FROM sessions s\n"
        "         JOIN design_gaps g\n"
        "           ON g.id = SUBSTR(s.title, LENGTH('discussion-of-gap:') + 1)\n"
        "        WHERE s.workspace_id = ?\n"
        "          AND s.title LIKE 'discussion-of-gap:%'\n"
        "          AND s.scene = ?\n"
        "          AND (g.status IS NULL OR g.status NOT IN ('closed', 'converged', 'dismissed', 'resolved'))\n"
        "        ORDER BY s.started_at DESC LIMIT 1",
        {workspace_id, scene});
    if (!row) return std::nullopt;
    return row->opt_str("gap_id");
}

std::string format_directives_block(const std::vector<std::string>& directives) {
    if (directives.empty()) return "";
    std::vector<std::string> lines{"【進行役への調整指示 (参加者からのリクエスト — 以後の進行/発話で必ず反映する)】"};
    for (const auto& d : directives) lines.push_back("- " + d);
    lines.push_back("※ 議題そのものは変えない。 あくまで議論の進め方・トーン・語り口の調整指示として従う。");
    return str::join(lines, "\n");
}

std::string strip_bot_mention(const std::string& content, const std::optional<std::string>& bot_id) {
    std::string text = content;
    if (bot_id && !bot_id->empty()) {
        text = str::replace_all(text, "<@!" + *bot_id + ">", " ");
        text = str::replace_all(text, "<@" + *bot_id + ">", " ");
    }
    return str::squish(text);
}

std::optional<std::string> normalize_directive_text(const std::string& text) {
    const std::string trimmed = str::trim(text);
    if (trimmed.empty()) return std::nullopt;
    if (str::js_length(trimmed) > DIRECTIVE_TEXT_CAP)
        return str::js_slice(trimmed, 0, static_cast<std::int64_t>(DIRECTIVE_TEXT_CAP) - 1) + "…";
    return trimmed;
}

} // namespace di::discord_hook
