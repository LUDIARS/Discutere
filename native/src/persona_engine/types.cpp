#include "persona_engine/types.hpp"

namespace di::persona_engine {

namespace {
template <class T>
Json opt(const std::optional<T>& v) {
    return v ? Json(*v) : Json(nullptr);
}
} // namespace

Json to_json(const PersonaRow& r) {
    return Json{{"id", r.id},
                {"name", r.name},
                {"display_name", r.display_name},
                {"description", r.description},
                {"traits", r.traits},
                {"speech_style", r.speech_style},
                {"skill_template", r.skill_template},
                {"learned_notes", r.learned_notes},
                {"created_at", r.created_at},
                {"updated_at", r.updated_at}};
}

Json to_json(const PersonaAssignmentRow& r) {
    return Json{{"id", r.id},
                {"persona_id", r.persona_id},
                {"session_id", r.session_id},
                {"assigned_at", r.assigned_at},
                {"released_at", opt(r.released_at)}};
}

Json to_json(const PersonaFeedbackRow& r) {
    return Json{{"id", r.id},       {"persona_id", r.persona_id}, {"session_id", opt(r.session_id)}, {"ts", r.ts},
                {"kind", r.kind},   {"delta", r.delta},           {"detail", opt(r.detail)}};
}

Json to_json(const RuleRow& r) {
    return Json{{"id", r.id},
                {"description", opt(r.description)},
                {"trigger_type", r.trigger_type},
                {"tick_sec", opt(r.tick_sec)},
                {"event_kind", opt(r.event_kind)},
                {"conditions", r.conditions},
                {"instructions", r.instructions},
                {"target", opt(r.target)},
                {"cooldown_sec", r.cooldown_sec},
                {"last_fired_at", opt(r.last_fired_at)},
                {"enabled", r.enabled},
                {"added_at", r.added_at},
                {"added_by", r.added_by},
                {"removed_at", opt(r.removed_at)},
                {"removed_by", opt(r.removed_by)},
                {"removed_reason", opt(r.removed_reason)}};
}

Json to_json(const RuleLogRow& r) {
    return Json{{"id", r.id},         {"ts", r.ts},         {"rule_id", opt(r.rule_id)},
                {"action", r.action}, {"detail", opt(r.detail)}, {"actor", r.actor}};
}

Json to_json(const PersonaSeed& s) {
    return Json{{"id", s.id},
                {"name", s.name},
                {"display_name", s.display_name},
                {"description", s.description},
                {"traits", s.traits},
                {"speech_style", s.speech_style}};
}

Json to_json(const RuleSeed& s) {
    // TS のオブジェクトと同じく、未指定 (undefined) のキーは出さない。
    Json j = Json{{"id", s.id}};
    if (s.description) j["description"] = *s.description;
    j["trigger_type"] = s.trigger_type;
    if (s.tick_sec) j["tick_sec"] = *s.tick_sec;
    if (s.event_kind) j["event_kind"] = *s.event_kind;
    j["instructions"] = s.instructions;
    if (s.target) j["target"] = *s.target;
    if (s.cooldown_sec) j["cooldown_sec"] = *s.cooldown_sec;
    return j;
}

Json token_usage_to_json(const TokenUsage& u) {
    Json j = Json::object();
    if (u.input_tokens) j["input_tokens"] = *u.input_tokens;
    if (u.output_tokens) j["output_tokens"] = *u.output_tokens;
    if (u.cache_read_input_tokens) j["cache_read_input_tokens"] = *u.cache_read_input_tokens;
    if (u.cache_creation_input_tokens) j["cache_creation_input_tokens"] = *u.cache_creation_input_tokens;
    if (u.cost_usd) j["cost_usd"] = *u.cost_usd;
    if (u.model) j["model"] = *u.model;
    return j;
}

PersonaRow persona_row_from(const sql::Row& row) {
    PersonaRow r;
    r.id = row.str("id");
    r.name = row.str("name");
    r.display_name = row.str("display_name");
    r.description = row.str("description");
    r.traits = row.str("traits");
    r.speech_style = row.str("speech_style");
    r.skill_template = row.str("skill_template");
    r.learned_notes = row.str("learned_notes");
    r.created_at = row.i64("created_at");
    r.updated_at = row.i64("updated_at");
    return r;
}

PersonaAssignmentRow persona_assignment_row_from(const sql::Row& row) {
    PersonaAssignmentRow r;
    r.id = row.i64("id");
    r.persona_id = row.str("persona_id");
    r.session_id = row.str("session_id");
    r.assigned_at = row.i64("assigned_at");
    r.released_at = row.opt_i64("released_at");
    return r;
}

PersonaFeedbackRow persona_feedback_row_from(const sql::Row& row) {
    PersonaFeedbackRow r;
    r.id = row.i64("id");
    r.persona_id = row.str("persona_id");
    r.session_id = row.opt_str("session_id");
    r.ts = row.i64("ts");
    r.kind = row.str("kind");
    r.delta = row.str("delta");
    r.detail = row.opt_str("detail");
    return r;
}

RuleRow rule_row_from(const sql::Row& row) {
    RuleRow r;
    r.id = row.str("id");
    r.description = row.opt_str("description");
    r.trigger_type = row.str("trigger_type");
    r.tick_sec = row.opt_i64("tick_sec");
    r.event_kind = row.opt_str("event_kind");
    r.conditions = row.str("conditions");
    r.instructions = row.str("instructions");
    r.target = row.opt_str("target");
    r.cooldown_sec = row.i64("cooldown_sec");
    r.last_fired_at = row.opt_i64("last_fired_at");
    r.enabled = row.i64("enabled");
    r.added_at = row.i64("added_at");
    r.added_by = row.str("added_by");
    r.removed_at = row.opt_i64("removed_at");
    r.removed_by = row.opt_str("removed_by");
    r.removed_reason = row.opt_str("removed_reason");
    return r;
}

RuleLogRow rule_log_row_from(const sql::Row& row) {
    RuleLogRow r;
    r.id = row.i64("id");
    r.ts = row.i64("ts");
    r.rule_id = row.opt_str("rule_id");
    r.action = row.str("action");
    r.detail = row.opt_str("detail");
    r.actor = row.str("actor");
    return r;
}

} // namespace di::persona_engine
