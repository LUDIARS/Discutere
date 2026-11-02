#pragma once
// コマンド処理の共通型と補助 (src/core/projection/command-handlers/types.ts)。
#include <optional>
#include <string>
#include <vector>

#include "base/json.hpp"
#include "core/projection/types.hpp"

namespace di::core::projection::command_handlers {

struct HandlerContext {
    ReturnTypeCreateCore* core = nullptr;
    std::string workspaceId;
    std::string sessionId;
    std::optional<std::string> personId;
    std::vector<std::string> args;
};

/** { ok: true, message } | { ok: false, error } */
struct HandlerResult {
    bool ok = false;
    std::string message;  // ok のとき
    std::string error;    // !ok のとき

    static HandlerResult success(std::string message) { return {true, std::move(message), {}}; }
    static HandlerResult failure(std::string error) { return {false, {}, std::move(error)}; }
};
Json to_json(const HandlerResult& r);

/** 最新 (updated_at 降順の先頭) の hypothesis id。無ければ nullopt。 */
std::optional<std::string> latest_hypothesis_id(ReturnTypeCreateCore& core, const std::string& workspace_id);

/** SessionUpdated を積んで session の mode / scene を変える。 */
void set_session_mode(ReturnTypeCreateCore& core, const std::string& session_id, const std::string& workspace_id,
                      const std::string& mode, const std::optional<std::string>& scene = std::nullopt);

/** id を新しく振ってイベントを積む。 */
void append_event(ReturnTypeCreateCore& core, const std::string& event_type, const Json& payload);

// ─── 局所的な補助 (TS の式をそのまま写すため) ───────────────────────

/** JS のテンプレートリテラル `${v}` (NULL は "null"、数は String(n))。 */
std::string js_template(const sql::Value& v);
/** JS の `!!v` (NULL / 0 / 空文字は false)。 */
bool js_truthy(const sql::Value& v);
/** `v ?? undefined` を payload に入れる (NULL なら key を出さない)。 */
void put_if_not_null(Json& payload, const char* key, const sql::Value& v);
/** JSON.parse(h.refs_json ?? "[]") (壊れていれば TS と同じく投げる。上限つき)。 */
Json parse_refs_json(const sql::Value& refs_json);

} // namespace di::core::projection::command_handlers
