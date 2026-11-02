#include "persona_engine/db/personas_repo.hpp"

#include <stdexcept>

#include "base/time.hpp"

namespace di::persona_engine::db {

namespace {
std::int64_t now_sec() { return time::now_ms() / 1000; }

bool is_limit_error(const std::string& error) {
    return error == "json: input too large" || error == "json: nesting too deep" || error == "json: too many elements";
}

Json safe_json_array(const std::string& raw) {
    std::string error;
    auto parsed = parse_json(raw, learned_notes_limits(), &error);
    if (!parsed) {
        // 上限超えは「壊れている」のではなく大きいだけ。[] にして上書きすると学習メモが消えるので拒否する。
        if (is_limit_error(error)) throw std::runtime_error("learned_notes exceeds limit");
        return Json::array();
    }
    return parsed->is_array() ? std::move(*parsed) : Json::array();
}
} // namespace

JsonLimits learned_notes_limits() {
    JsonLimits l;
    l.max_bytes = 4u << 20;
    l.max_elements = 200'000;
    l.max_depth = 32;
    return l;
}

PersonasRepo::PersonasRepo(sql::DatabasePtr db) : db_(std::move(db)) {}

void PersonasRepo::insert_or_ignore(const PersonaSeed& seed) {
    const auto now = now_sec();
    db_->run(R"(INSERT OR IGNORE INTO personas
          (id, name, display_name, description, traits, speech_style, skill_template, learned_notes, created_at, updated_at)
         VALUES (?, ?, ?, ?, ?, ?, '', '[]', ?, ?))",
             {seed.id, seed.name, seed.display_name, seed.description, json_stringify(Json(seed.traits)), seed.speech_style,
              now, now});
}

void PersonasRepo::bulk_seed(const std::vector<PersonaSeed>& seeds) {
    db_->transaction([&] {
        for (const auto& row : seeds) insert_or_ignore(row);
    });
}

std::optional<PersonaRow> PersonasRepo::get(const std::string& id) {
    auto row = db_->get("SELECT * FROM personas WHERE id = ?", {id});
    if (!row) return std::nullopt;
    return persona_row_from(*row);
}

std::vector<PersonaRow> PersonasRepo::list() {
    std::vector<PersonaRow> out;
    for (const auto& row : db_->all("SELECT * FROM personas ORDER BY id")) out.push_back(persona_row_from(row));
    return out;
}

PersonaAssignmentRow PersonasRepo::assign(const std::string& persona_id, const std::string& session_id) {
    const auto info = db_->run("INSERT INTO persona_assignments (persona_id, session_id, assigned_at) VALUES (?, ?, ?)",
                               {persona_id, session_id, now_sec()});
    auto row = db_->get("SELECT * FROM persona_assignments WHERE id = ?", {info.last_insert_rowid});
    if (!row) throw std::runtime_error("persona assignment not found after insert");
    return persona_assignment_row_from(*row);
}

void PersonasRepo::release(std::int64_t assignment_id) {
    db_->run("UPDATE persona_assignments SET released_at = ? WHERE id = ?", {now_sec(), assignment_id});
}

std::vector<PersonaAssignmentRow> PersonasRepo::active_assignments_for_session(const std::string& session_id) {
    std::vector<PersonaAssignmentRow> out;
    for (const auto& row : db_->all(
             "SELECT * FROM persona_assignments WHERE session_id = ? AND released_at IS NULL ORDER BY assigned_at", {session_id}))
        out.push_back(persona_assignment_row_from(row));
    return out;
}

PersonaFeedbackRow PersonasRepo::add_feedback(const AddFeedbackInput& input) {
    const auto info = db_->run(
        "INSERT INTO persona_feedback (persona_id, session_id, ts, kind, delta, detail) VALUES (?, ?, ?, ?, ?, ?)",
        {input.persona_id, input.session_id, now_sec(), input.kind, input.delta, input.detail});
    auto row = db_->get("SELECT * FROM persona_feedback WHERE id = ?", {info.last_insert_rowid});
    if (!row) throw std::runtime_error("persona feedback not found after insert");
    return persona_feedback_row_from(*row);
}

std::vector<PersonaFeedbackRow> PersonasRepo::feedback_for(const std::string& persona_id, std::int64_t limit) {
    std::vector<PersonaFeedbackRow> out;
    for (const auto& row : db_->all("SELECT * FROM persona_feedback WHERE persona_id = ? ORDER BY ts DESC LIMIT ?",
                                    {persona_id, limit}))
        out.push_back(persona_feedback_row_from(row));
    return out;
}

void PersonasRepo::append_learned_note(const std::string& persona_id, const std::string& note) {
    const auto row = get(persona_id);
    if (!row) throw std::runtime_error("persona not found: " + persona_id);
    Json arr = safe_json_array(row->learned_notes);
    arr.push_back(Json{{"note", note}, {"ts", now_sec()}});
    db_->run("UPDATE personas SET learned_notes = ?, updated_at = ? WHERE id = ?", {json_stringify(arr), now_sec(), persona_id});
}

} // namespace di::persona_engine::db
