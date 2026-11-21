// @implements SPEC-DI-PLAYTEST-PROPOSALS
#include "playtest/proposal_outbox.hpp"

#include <algorithm>

#include "base/crypto.hpp"
#include "base/log.hpp"
#include "base/strings.hpp"

namespace di::playtest {
namespace {

bool ascii_alnum(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

std::string target_key(const std::string& game) {
    if (!game.empty() && game.size() <= 200 && ascii_alnum(game.front()) &&
        std::all_of(game.begin(), game.end(), [](char c) { return ascii_alnum(c) || std::string_view("._:/#-").find(c) != std::string_view::npos; }))
        return game;
    return "game:" + crypto::sha256_hex(game);
}

bool valid_project_code(const std::string& code) {
    return !code.empty() && code.size() <= 64 && ascii_alnum(code.front()) &&
           std::all_of(code.begin(), code.end(), [](char c) { return ascii_alnum(c) || c == '.' || c == '_' || c == '-'; });
}

} // namespace

void ensure_proposal_outbox(sql::Database& db) {
    db.exec(R"SQL(CREATE TABLE IF NOT EXISTS playtest_proposal_outbox (
      gap_id TEXT PRIMARY KEY, target_key TEXT NOT NULL, summary TEXT NOT NULL, observed_at INTEGER NOT NULL,
      project_code TEXT, attempts INTEGER NOT NULL DEFAULT 0, next_attempt_at INTEGER NOT NULL DEFAULT 0,
      last_error TEXT, delivered_at INTEGER))SQL");
    db.exec(R"SQL(CREATE INDEX IF NOT EXISTS playtest_proposal_pending ON playtest_proposal_outbox(next_attempt_at)
                  WHERE delivered_at IS NULL)SQL");
}

void enqueue_playtest_proposal(sql::Database& db, const std::string& gap_id, const std::string& summary, std::int64_t now) {
    const auto gap = db.get("SELECT game_id, evidence_json FROM design_gaps WHERE id = ?", {gap_id});
    if (!gap || gap->str("game_id").empty()) return;
    const auto evidence = parse_json(gap->str("evidence_json"));
    if (!evidence || json_string(*evidence, "source") != "auto:playtest-voices") return;
    const auto text = str::trim(str::js_slice(str::trim(summary), 0, 2'000));
    if (text.empty()) {
        log::warn("[playtest-proposal] empty conclusion " + gap_id);
        return;
    }
    ensure_proposal_outbox(db);
    db.run(R"SQL(INSERT INTO playtest_proposal_outbox(gap_id, target_key, summary, observed_at) VALUES (?, ?, ?, ?)
                 ON CONFLICT(gap_id) DO NOTHING)SQL", {gap_id, target_key(gap->str("game_id")), text, now});
}

void deliver_next_proposal(sql::Database& db, const ProposalDestination& destination, std::int64_t now, const ProposalFetch& fetch) {
    const auto row = db.get(R"SQL(SELECT p.* FROM playtest_proposal_outbox p JOIN design_gaps g ON g.id = p.gap_id
       WHERE p.delivered_at IS NULL AND p.next_attempt_at <= ? AND g.status IN ('closed', 'converged')
       ORDER BY p.next_attempt_at, p.observed_at, p.gap_id LIMIT 1)SQL", {now});
    if (!row) return;
    const auto id = row->str("gap_id");
    auto project = row->opt_str("project_code").value_or(destination.projectCode);
    std::optional<std::string> error;
    if (destination.baseUrl.empty()) {
        error = "management_url_not_configured";
    } else if (project.empty()) {
        error = "management_project_not_configured";
    } else if (!valid_project_code(project)) {
        error = "invalid_management_project_code";
    } else {
        // Freeze routing only once there is a request to make. A retry can reuse the exact event after a restart.
        const auto frozen = db.get("UPDATE playtest_proposal_outbox SET project_code = COALESCE(project_code, ?) "
                                   "WHERE gap_id = ? AND delivered_at IS NULL RETURNING project_code", {project, id});
        if (!frozen) return;
        project = frozen->str("project_code");
        error = send_proposal(destination.baseUrl,
                              proposal_payload(id, project, row->str("target_key"), row->str("summary"), row->i64("observed_at")), fetch);
    }
    if (error) {
        db.run("UPDATE playtest_proposal_outbox SET attempts = attempts + 1, last_error = ?, next_attempt_at = ? "
               "WHERE gap_id = ? AND delivered_at IS NULL", {*error, now + 60'000, id});
        log::warn("[playtest-proposal] " + id + " " + *error);
    } else {
        db.run("UPDATE playtest_proposal_outbox SET attempts = attempts + 1, last_error = NULL, delivered_at = ? "
               "WHERE gap_id = ? AND delivered_at IS NULL", {now, id});
    }
}

} // namespace di::playtest
