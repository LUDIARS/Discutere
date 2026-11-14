// GET /api/flow/:session/status (flow/web/routes.cpp) から切り出した DB の読み取り。JSON の形・順序は status と同じ。
#include "flow/transcript/snapshot.hpp"

#include <cstdint>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include "flow/db/connection.hpp"
#include "flow/discussion_paper.hpp"
#include "flow/persona_display.hpp"

namespace di::flow::transcript {

namespace {

/** DB の JSON 列 (top_utterance_ids_json / aufhebung_json) を読む上限。 */
const JsonLimits kDbJsonLimits{1u << 20, 50'000, 16};

/** 1 議論ぶんの行を読む上限。 */
const sql::QueryLimits kRowLimits{200'000, 256u << 20};

std::vector<std::string> string_array_column(const std::string& raw) {
    std::vector<std::string> out;
    const auto parsed = parse_json(raw, kDbJsonLimits);
    if (parsed && parsed->is_array())
        for (const auto& v : *parsed)
            if (v.is_string()) out.push_back(v.get<std::string>());
    return out;
}

/** 発話ごとの印 (投票数・採択・止揚) の材料。 */
struct MarkIndex {
    std::map<std::string, std::int64_t> voteById;
    std::set<std::string> winnerIds;
    std::map<std::int64_t, std::vector<std::string>> aufhebungByRound;

    /** out に votes / isWinner / roundAufhebung を書く。何か印があれば true。 */
    bool apply(Json& out, const std::string& id, std::int64_t round) const {
        const auto v = voteById.find(id);
        const std::int64_t votes = v == voteById.end() ? 0 : v->second;
        const bool is_winner = winnerIds.count(id) > 0;
        Json auf = Json::array();
        // 止揚はラウンド全体ではなく、世論として使われた意見だけに付ける。
        if (is_winner) {
            const auto a = aufhebungByRound.find(round);
            if (a != aufhebungByRound.end()) auf = a->second;
        }
        out["votes"] = votes;
        out["isWinner"] = is_winner;
        out["roundAufhebung"] = auf;
        return votes > 0 || is_winner || !auf.empty();
    }
};

MarkIndex load_mark_index(sql::Database& db, const std::string& session_id) {
    MarkIndex idx;
    const auto vote_rows = db.all(
        "SELECT round, chosen_utterance_id AS id, COUNT(*) AS votes\n"
        "         FROM vote\n"
        "        WHERE session_id = ? AND chosen_utterance_id IS NOT NULL\n"
        "        GROUP BY round, chosen_utterance_id\n"
        "        ORDER BY round ASC",
        {session_id}, kRowLimits);
    std::map<std::int64_t, std::pair<std::string, std::int64_t>> vote_winner_by_round;
    for (const auto& r : vote_rows) {
        const auto id = r.str("id");
        const auto votes = r.i64("votes");
        idx.voteById[id] += votes;
        const auto round = r.i64("round");
        const auto cur = vote_winner_by_round.find(round);
        if (cur == vote_winner_by_round.end() || votes > cur->second.second) vote_winner_by_round[round] = {id, votes};
    }
    const auto score_winner_rows = db.all(
        "SELECT round, utterance_id AS id\n"
        "         FROM improvement_score\n"
        "        WHERE session_id = ? AND is_winner = 1",
        {session_id}, kRowLimits);
    const auto top_row = db.get("SELECT top_utterance_ids_json FROM flow_conclusion WHERE session_id = ?", {session_id});
    if (top_row)
        for (auto& id : string_array_column(top_row->str("top_utterance_ids_json"))) idx.winnerIds.insert(std::move(id));
    for (const auto& [round, w] : vote_winner_by_round)
        if (w.second > 0) idx.winnerIds.insert(w.first);
    for (const auto& r : score_winner_rows) idx.winnerIds.insert(r.str("id"));
    const auto round_rows = db.all(
        "SELECT round, aufhebung_json FROM discussion_paper_round WHERE paper_id IN (SELECT id FROM discussion_paper WHERE session_id = ?)",
        {session_id}, kRowLimits);
    for (const auto& r : round_rows) idx.aufhebungByRound[r.i64("round")] = string_array_column(r.str("aufhebung_json"));
    return idx;
}

} // namespace

TranscriptSnapshot load_transcript_snapshot(const std::string& session_id, double since) {
    auto db = db::get_flow_db();
    TranscriptSnapshot snap;

    const auto utterances = db->all(
        "SELECT id, round, turn, persona_name, role, stance, possession_name, text, is_error, created_at\n"
        "         FROM flow_utterance\n"
        "        WHERE session_id = ? AND created_at > ?\n"
        "        ORDER BY created_at ASC",
        {session_id, since}, kRowLimits);
    const MarkIndex marks = load_mark_index(*db, session_id);
    const auto mark_rows = db->all("SELECT id, round FROM flow_utterance WHERE session_id = ?", {session_id}, kRowLimits);
    const auto conclusion_row = db->get("SELECT summary, concluded FROM flow_conclusion WHERE session_id = ?", {session_id});

    for (const auto& u : utterances) {
        Json j = Json::object();
        const std::string id = u.str("id");
        j["id"] = id;
        j["round"] = u["round"].to_json();
        j["turn"] = u["turn"].to_json();
        j["personaName"] = u.str("persona_name");
        // 「名前 (ロール/憑依ペルソナ)」(item2/4)。憑依なしは括弧内ロールのみ。
        DisplayNameInput dn;
        dn.name = u.str("persona_name");
        dn.stance = u.opt_str("stance");
        dn.role = u.opt_str("role");
        dn.possessionName = u.opt_str("possession_name");
        j["displayName"] = compose_display_name(dn);
        j["role"] = u["role"].to_json();
        j["stance"] = u["stance"].to_json();
        j["possessionName"] = u["possession_name"].to_json();
        j["text"] = u["text"].to_json();
        j["isError"] = u.i64("is_error") == 1;
        marks.apply(j, id, u.i64("round"));
        j["createdAt"] = u["created_at"].to_json();
        snap.utterances.push_back(std::move(j));
    }
    for (const auto& r : mark_rows) {
        Json m = Json::object();
        const std::string id = r.str("id");
        m["id"] = id;
        if (marks.apply(m, id, r.i64("round"))) snap.marks.push_back(std::move(m));
    }
    snap.hasConclusionRow = conclusion_row.has_value();
    snap.conclusion = conclusion_row ? (*conclusion_row)["summary"].to_json() : Json(nullptr);
    snap.concluded = conclusion_row && conclusion_row->i64("concluded") == 1;
    snap.paperMd = get_paper_body_by_session(session_id);
    return snap;
}

} // namespace di::flow::transcript
