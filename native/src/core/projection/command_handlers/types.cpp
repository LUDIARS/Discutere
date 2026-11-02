#include "core/projection/command_handlers/types.hpp"

#include <stdexcept>

#include "base/crypto.hpp"
#include "base/time.hpp"
#include "core/events/event_types.hpp"

namespace di::core::projection::command_handlers {

namespace {
/** refs_json の上限 (DB の値も外部入力)。 */
constexpr JsonLimits kRefsJsonLimits{4u << 20, 200'000, 16};
} // namespace

Json to_json(const HandlerResult& r) {
    Json j = Json::object();
    j["ok"] = r.ok;
    if (r.ok)
        j["message"] = r.message;
    else
        j["error"] = r.error;
    return j;
}

std::optional<std::string> latest_hypothesis_id(ReturnTypeCreateCore& core, const std::string& workspace_id) {
    auto rows = core.raw().all("SELECT id FROM hypotheses WHERE workspace_id = ? ORDER BY updated_at DESC LIMIT 1", {workspace_id});
    if (rows.empty()) return std::nullopt;
    return rows[0].opt_str("id");
}

void set_session_mode(ReturnTypeCreateCore& core, const std::string& session_id, const std::string& workspace_id,
                      const std::string& mode, const std::optional<std::string>& scene) {
    const auto s = core.repos.session.get(session_id);
    Json payload = Json::object();
    payload["id"] = session_id;
    payload["workspaceId"] = workspace_id;
    payload["title"] = (s && !s->is_null("title")) ? (*s)["title"].to_json() : Json("session");
    payload["startedAt"] = (s && !s->is_null("started_at")) ? (*s)["started_at"].to_json() : Json(time::now_ms());
    if (s) put_if_not_null(payload, "endedAt", (*s)["ended_at"]);
    payload["mode"] = mode;
    if (scene) payload["scene"] = *scene;
    core.eventLog.append_event({std::nullopt, std::string(events::EVENT_TYPES::SessionUpdated), payload, std::nullopt});
}

void append_event(ReturnTypeCreateCore& core, const std::string& event_type, const Json& payload) {
    core.eventLog.append_event({crypto::uuid_v4(), event_type, payload, std::nullopt});
}

std::string js_template(const sql::Value& v) {
    if (v.is_null() || v.is_blob()) return v.is_null() ? "null" : std::string();
    return v.as_string();
}

bool js_truthy(const sql::Value& v) {
    if (v.is_null()) return false;
    if (v.is_text()) return !v.as_string().empty();
    if (v.is_number()) return v.as_double() != 0;
    return true;
}

void put_if_not_null(Json& payload, const char* key, const sql::Value& v) {
    if (!v.is_null()) payload[key] = v.to_json();
}

Json parse_refs_json(const sql::Value& refs_json) {
    const std::string text = refs_json.is_null() ? std::string("[]") : refs_json.as_string();
    std::string error;
    auto parsed = parse_json(text, kRefsJsonLimits, &error);
    if (!parsed) throw std::runtime_error("refs_json を読めない: " + error);
    return *parsed;
}

} // namespace di::core::projection::command_handlers
