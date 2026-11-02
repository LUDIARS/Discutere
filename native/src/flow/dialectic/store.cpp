#include "flow/dialectic/store.hpp"

#include <algorithm>

#include "base/crypto.hpp"
#include "base/time.hpp"
#include "flow/db/connection.hpp"

namespace di::flow::dialectic {

namespace {

JsonLimits stored_json_limits() { return JsonLimits{16u << 20, 1'000'000, 64}; }

std::optional<Json> parse_stored(const std::string& s) { return parse_json(s, stored_json_limits()); }

std::vector<std::string> string_list(const Json& v) {
    std::vector<std::string> out;
    if (v.is_array())
        for (const auto& s : v)
            if (s.is_string()) out.push_back(s.get<std::string>());
    return out;
}

std::vector<std::string> parse_string_list(const std::string& s) {
    auto v = parse_stored(s);
    return v ? string_list(*v) : std::vector<std::string>{};
}

std::string grounds_json(const std::vector<Ground>& grounds) {
    Json arr = Json::array();
    for (const auto& g : grounds) arr.push_back(to_json(g));
    return json_stringify(arr);
}

std::string ratification_json(const std::vector<RatificationEntry>& r) {
    Json arr = Json::array();
    for (const auto& e : r) arr.push_back(to_json(e));
    return json_stringify(arr);
}

bool one_of(const std::string& v, std::initializer_list<const char*> list) {
    return std::any_of(list.begin(), list.end(), [&](const char* s) { return v == s; });
}

} // namespace

Json to_json(const Ground& g) {
    Json o = Json::object();
    o["id"] = g.id;
    o["text"] = g.text;
    o["state"] = g.state;
    if (g.kind) o["kind"] = *g.kind;
    if (g.warrant) o["warrant"] = *g.warrant;
    if (g.claimKind) o["claimKind"] = *g.claimKind;
    if (g.evidenceIds) o["evidenceIds"] = *g.evidenceIds;
    return o;
}

Ground ground_from_json(const Json& j) {
    Ground g;
    g.id = json_string(j, "id");
    g.text = json_string(j, "text");
    g.state = json_string(j, "state");
    g.kind = json_opt_string(j, "kind");
    g.warrant = json_opt_string(j, "warrant");
    g.claimKind = json_opt_string(j, "claimKind");
    if (j.is_object() && j.contains("evidenceIds") && j["evidenceIds"].is_array()) g.evidenceIds = string_list(j["evidenceIds"]);
    return g;
}

Json to_json(const RatificationEntry& r) {
    Json o = Json::object();
    o["positionId"] = r.positionId;
    o["verdict"] = r.verdict;
    o["preservedIds"] = r.preservedIds;
    o["missing"] = r.missing;
    o["reason"] = r.reason;
    return o;
}

RatificationEntry ratification_from_json(const Json& j) {
    RatificationEntry r;
    r.positionId = json_string(j, "positionId");
    r.verdict = json_string(j, "verdict");
    r.preservedIds = string_list(json_array(j, "preservedIds"));
    r.missing = string_list(json_array(j, "missing"));
    r.reason = json_string(j, "reason");
    return r;
}

IssueRecord insert_issue(IssueRecord issue) {
    if (issue.id.empty()) issue.id = crypto::uuid_v4();
    db::get_flow_db()->run(
        R"SQL(INSERT INTO flow_issue (id, session_id, title, ordinal, source, status, created_at)
       VALUES (?, ?, ?, ?, ?, ?, ?))SQL",
        {issue.id, issue.sessionId, issue.title, issue.ordinal, issue.source, issue.status, time::now_ms()});
    return issue;
}

void update_issue_status(const std::string& issue_id, const std::string& status) {
    db::get_flow_db()->run("UPDATE flow_issue SET status = ? WHERE id = ?", {status, issue_id});
}

std::vector<IssueRecord> list_issues(const std::string& session_id) {
    const auto rows = db::get_flow_db()->all(
        R"SQL(SELECT id, session_id, title, ordinal, source, status FROM flow_issue
        WHERE session_id = ? ORDER BY ordinal)SQL",
        {session_id});
    std::vector<IssueRecord> out;
    for (const auto& r : rows) {
        IssueRecord i;
        i.id = r.str("id");
        i.sessionId = r.str("session_id");
        i.title = r.str("title");
        i.ordinal = static_cast<int>(r.i64("ordinal"));
        i.source = r.str("source") == "paper-gate" ? "paper-gate" : "llm";
        i.status = r.str("status") == "concluded" ? "concluded" : "open";
        out.push_back(std::move(i));
    }
    return out;
}

PositionRecord insert_position(PositionRecord p) {
    if (p.id.empty()) p.id = crypto::uuid_v4();
    db::get_flow_db()->run(
        R"SQL(INSERT INTO flow_position (id, issue_id, persona_id, stance, claim, qualifier, grounds_json, values_json, created_at)
       VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?))SQL",
        {p.id, p.issueId, p.personaId, p.stance, p.claim, sql::Value(p.qualifier), grounds_json(p.grounds),
         json_stringify(Json(p.values)), time::now_ms()});
    return p;
}

void update_position_grounds(const std::string& position_id, const std::vector<Ground>& grounds) {
    db::get_flow_db()->run("UPDATE flow_position SET grounds_json = ? WHERE id = ?", {grounds_json(grounds), position_id});
}

std::vector<PositionRecord> list_positions(const std::string& issue_id) {
    const auto rows = db::get_flow_db()->all(
        R"SQL(SELECT id, issue_id, persona_id, stance, claim, qualifier, grounds_json, values_json
         FROM flow_position WHERE issue_id = ? ORDER BY created_at, id)SQL",
        {issue_id});
    std::vector<PositionRecord> out;
    for (const auto& r : rows) {
        PositionRecord p;
        p.id = r.str("id");
        p.issueId = r.str("issue_id");
        p.personaId = r.str("persona_id");
        p.stance = r.str("stance") == "con" ? "con" : "pro";
        p.claim = r.str("claim");
        p.qualifier = r.opt_str("qualifier");
        if (auto g = parse_stored(r.str("grounds_json")); g && g->is_array())
            for (const auto& item : *g) p.grounds.push_back(ground_from_json(item));
        p.values = parse_string_list(r.str("values_json"));
        out.push_back(std::move(p));
    }
    return out;
}

TensionRecord insert_tension(TensionRecord t) {
    if (t.id.empty()) t.id = crypto::uuid_v4();
    db::get_flow_db()->run(
        R"SQL(INSERT INTO flow_tension (id, issue_id, position_a, position_b, type, status, resolution_note, created_at)
       VALUES (?, ?, ?, ?, ?, ?, ?, ?))SQL",
        {t.id, t.issueId, t.positionA, t.positionB, t.type, t.status, sql::Value(t.resolutionNote), time::now_ms()});
    return t;
}

void update_tension_status(const std::string& tension_id, const TensionStatus& status) {
    db::get_flow_db()->run("UPDATE flow_tension SET status = ? WHERE id = ?", {status, tension_id});
}

void update_tension_status(const std::string& tension_id, const TensionStatus& status,
                           const std::optional<std::string>& resolution_note) {
    db::get_flow_db()->run("UPDATE flow_tension SET status = ?, resolution_note = ? WHERE id = ?",
                           {status, sql::Value(resolution_note), tension_id});
}

std::vector<TensionRecord> list_tensions(const std::string& issue_id) {
    const auto rows = db::get_flow_db()->all(
        R"SQL(SELECT id, issue_id, position_a, position_b, type, status, resolution_note
         FROM flow_tension WHERE issue_id = ? ORDER BY created_at, id)SQL",
        {issue_id});
    std::vector<TensionRecord> out;
    for (const auto& r : rows) {
        TensionRecord t;
        t.id = r.str("id");
        t.issueId = r.str("issue_id");
        t.positionA = r.str("position_a");
        t.positionB = r.str("position_b");
        const std::string type = r.str("type");
        t.type = one_of(type, {"facts", "values", "means", "tradeoff"}) ? type : "values";
        const std::string status = r.str("status");
        t.status = one_of(status, {"open", "resolved", "synthesized", "compromised", "agreed_disagree", "unresolved_fact",
                                   "agreed", "aligned", "partially_aligned"})
                       ? status
                       : "open";
        t.resolutionNote = r.opt_str("resolution_note");
        out.push_back(std::move(t));
    }
    return out;
}

SynthesisRecord insert_synthesis(SynthesisRecord s) {
    if (s.id.empty()) s.id = crypto::uuid_v4();
    db::get_flow_db()->run(
        R"SQL(INSERT INTO flow_synthesis
         (id, tension_id, preserves_json, negates_json, elevates, kind, text, ratification_json, status, revision, created_at)
       VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?))SQL",
        {s.id, s.tensionId, json_stringify(Json(s.preserves)), json_stringify(Json(s.negates)), sql::Value(s.elevates),
         s.kind, s.text, ratification_json(s.ratification), s.status, s.revision, time::now_ms()});
    return s;
}

void update_synthesis(const std::string& id, const SynthesisRecord& patch) {
    db::get_flow_db()->run(
        R"SQL(UPDATE flow_synthesis
          SET preserves_json = ?, negates_json = ?, elevates = ?, kind = ?, text = ?,
              ratification_json = ?, status = ?, revision = ?
        WHERE id = ?)SQL",
        {json_stringify(Json(patch.preserves)), json_stringify(Json(patch.negates)), sql::Value(patch.elevates), patch.kind,
         patch.text, ratification_json(patch.ratification), patch.status, patch.revision, id});
}

std::vector<SynthesisRecord> list_syntheses(const std::string& tension_id) {
    const auto rows = db::get_flow_db()->all(
        R"SQL(SELECT id, tension_id, preserves_json, negates_json, elevates, kind, text, ratification_json, status, revision
         FROM flow_synthesis WHERE tension_id = ? ORDER BY created_at, id)SQL",
        {tension_id});
    std::vector<SynthesisRecord> out;
    for (const auto& r : rows) {
        SynthesisRecord s;
        s.id = r.str("id");
        s.tensionId = r.str("tension_id");
        s.preserves = parse_string_list(r.str("preserves_json"));
        s.negates = parse_string_list(r.str("negates_json"));
        s.elevates = r.opt_str("elevates");
        s.kind = r.str("kind") == "aufheben" ? "aufheben" : "compromise";
        s.text = r.str("text");
        if (auto v = parse_stored(r.str("ratification_json")); v && v->is_array())
            for (const auto& e : *v) s.ratification.push_back(ratification_from_json(e));
        const std::string status = r.str("status");
        s.status = one_of(status, {"ratified", "rejected", "revising"}) ? status : "rejected";
        s.revision = static_cast<int>(r.i64("revision"));
        out.push_back(std::move(s));
    }
    return out;
}

} // namespace di::flow::dialectic
