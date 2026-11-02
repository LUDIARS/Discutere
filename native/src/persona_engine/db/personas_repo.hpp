#pragma once
// src/persona-engine/db/personas-repo.ts — persona / assignment / feedback / learned_notes。
#include <optional>
#include <string>
#include <vector>

#include "base/sqlite.hpp"
#include "persona_engine/types.hpp"

namespace di::persona_engine::db {

struct AddFeedbackInput {
    std::string persona_id;
    std::optional<std::string> session_id;
    /** PersonaFeedbackRow::kind */
    std::string kind;
    std::string delta;
    std::optional<std::string> detail;
};

class PersonasRepo {
public:
    explicit PersonasRepo(sql::DatabasePtr db);

    // ─── persona CRUD ───
    void insert_or_ignore(const PersonaSeed& seed);
    void bulk_seed(const std::vector<PersonaSeed>& seeds);
    std::optional<PersonaRow> get(const std::string& id);
    std::vector<PersonaRow> list();

    // ─── assignment ───
    PersonaAssignmentRow assign(const std::string& persona_id, const std::string& session_id);
    void release(std::int64_t assignment_id);
    /** session で active な assignment (released_at IS NULL) を返す */
    std::vector<PersonaAssignmentRow> active_assignments_for_session(const std::string& session_id);

    // ─── feedback / learned_notes ───
    PersonaFeedbackRow add_feedback(const AddFeedbackInput& input);
    std::vector<PersonaFeedbackRow> feedback_for(const std::string& persona_id, std::int64_t limit = 50);
    /** learned_notes に 1 行追加 (JSON 配列に append)。persona が無ければ std::runtime_error。 */
    void append_learned_note(const std::string& persona_id, const std::string& note);

private:
    sql::DatabasePtr db_;
};

/** learned_notes 列を読む上限。壊れた JSON は [] (TS と同じ)、上限超えは消さないよう std::runtime_error。 */
JsonLimits learned_notes_limits();

} // namespace di::persona_engine::db
