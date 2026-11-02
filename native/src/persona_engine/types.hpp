#pragma once
// persona-engine 共通型定義 (src/persona-engine/types.ts)。
// Discutere 固有の型 (Hypothesis / Gap / Utterance 等) は context_provider.hpp に隔離する。
// TokenUsage は persona_engine/llm/client.hpp のものを使う (ここでは再定義しない)。
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.hpp"
#include "base/sqlite.hpp"
#include "persona_engine/llm/client.hpp"

namespace di::persona_engine {

struct PersonaRow {
    std::string id;
    std::string name;
    std::string display_name;
    std::string description;
    /** JSON 化された string[] */
    std::string traits;
    std::string speech_style;
    /** future use (Concordia 互換) */
    std::string skill_template;
    /** JSON 化された learning note 配列 */
    std::string learned_notes;
    std::int64_t created_at = 0;
    std::int64_t updated_at = 0;
};

struct PersonaAssignmentRow {
    std::int64_t id = 0;
    std::string persona_id;
    std::string session_id;
    std::int64_t assigned_at = 0;
    std::optional<std::int64_t> released_at;
};

struct PersonaFeedbackRow {
    std::int64_t id = 0;
    std::string persona_id;
    std::optional<std::string> session_id;
    std::int64_t ts = 0;
    /** "session-end" | "chat-update" | "manual" | "system" */
    std::string kind;
    std::string delta;
    std::optional<std::string> detail;
};

/** "tick" | "event" */
using RuleTriggerType = std::string;

struct RuleRow {
    std::string id;
    std::optional<std::string> description;
    RuleTriggerType trigger_type;
    std::optional<std::int64_t> tick_sec;
    std::optional<std::string> event_kind;
    /** JSON 化された condition 配列 (Phase 0 では未使用 = "[]") */
    std::string conditions;
    /** LLM に渡される主指示 */
    std::string instructions;
    /** 任意の対象 persona_id (rule fire 時に persona を絞る) */
    std::optional<std::string> target;
    std::int64_t cooldown_sec = 60;
    std::optional<std::int64_t> last_fired_at;
    std::int64_t enabled = 1;
    std::int64_t added_at = 0;
    std::string added_by;
    std::optional<std::int64_t> removed_at;
    std::optional<std::string> removed_by;
    std::optional<std::string> removed_reason;
};

struct RuleLogRow {
    std::int64_t id = 0;
    std::int64_t ts = 0;
    std::optional<std::string> rule_id;
    /** "add" | "remove" | "fire" | "skip" | "error" */
    std::string action;
    std::optional<std::string> detail;
    /** "system" | "ai" | "human" | "engine" */
    std::string actor;
};

struct PersonaSeed {
    std::string id;
    std::string name;
    std::string display_name;
    std::string description;
    std::vector<std::string> traits;
    std::string speech_style;
};

struct RuleSeed {
    std::string id;
    std::optional<std::string> description;
    RuleTriggerType trigger_type;
    std::optional<std::int64_t> tick_sec;
    std::optional<std::string> event_kind;
    std::string instructions;
    std::optional<std::string> target;
    std::optional<std::int64_t> cooldown_sec;
};

/** pino 互換の構造化ロガー境界 (meta + message)。 */
class Logger {
public:
    virtual ~Logger() = default;
    virtual void debug(const Json& meta, std::string_view message) = 0;
    virtual void info(const Json& meta, std::string_view message) = 0;
    virtual void warn(const Json& meta, std::string_view message) = 0;
    virtual void error(const Json& meta, std::string_view message) = 0;
};

// ─── JSON (DB 行と同じ列名・列順。better-sqlite3 が返す行オブジェクトと同じ形) ───
Json to_json(const PersonaRow& r);
Json to_json(const PersonaAssignmentRow& r);
Json to_json(const PersonaFeedbackRow& r);
Json to_json(const RuleRow& r);
Json to_json(const RuleLogRow& r);
Json to_json(const PersonaSeed& s);
Json to_json(const RuleSeed& s);
/** TokenUsage を JSON にする (値のあるキーだけ。順は types.ts の宣言順)。 */
Json token_usage_to_json(const TokenUsage& u);

// ─── DB 行の読み取り ───
PersonaRow persona_row_from(const sql::Row& row);
PersonaAssignmentRow persona_assignment_row_from(const sql::Row& row);
PersonaFeedbackRow persona_feedback_row_from(const sql::Row& row);
RuleRow rule_row_from(const sql::Row& row);
RuleLogRow rule_log_row_from(const sql::Row& row);

} // namespace di::persona_engine
