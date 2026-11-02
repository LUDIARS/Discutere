// @implements SPEC-PERSONA-BRIDGE-UTTERANCE-EXPORT
#include "persona_bridge/utterance_export.hpp"

#include <algorithm>
#include <cmath>

#include "base/crypto.hpp"
#include "base/strings.hpp"
#include "base/time.hpp"
#include "persona_bridge/authorization_assertion.hpp"

namespace di::persona_bridge {

namespace {

constexpr std::size_t MAX_CURSOR_LENGTH = 512;
constexpr std::size_t MAX_UTTERANCE_ID_LENGTH = 256;
constexpr double kMaxSafeInteger = 9007199254740991.0;
constexpr std::int64_t kMaxDateMs = 8'640'000'000'000'000LL;

bool is_cursor_text(const std::string& s) {
    return std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}

/** new Date(ms).toISOString() (範囲外は RangeError 相当で例外)。 */
std::string iso_from_ms(double ms) {
    if (!std::isfinite(ms) || std::fabs(ms) > static_cast<double>(kMaxDateMs))
        throw std::range_error("Invalid time value");
    return time::iso_utc(static_cast<std::int64_t>(std::trunc(ms)));
}

}  // namespace

Json to_json(const ExportedUtterance& u) {
    Json o = Json::object();
    o["text"] = u.text;
    o["createdAt"] = u.createdAt;
    return o;
}

Json to_json(const ExportableUtteranceRecord& u) {
    Json o = Json::object();
    o["id"] = u.id;
    o["text"] = u.text;
    o["createdAt"] = u.createdAt;
    return o;
}

std::string encode_utterance_cursor(const UtteranceCursor& cursor) {
    return crypto::base64url_encode(json_stringify(Json::array({cursor.postedAt, cursor.id})));
}

UtteranceCursor decode_utterance_cursor(const std::string& value) {
    if (value.empty() || value.size() > MAX_CURSOR_LENGTH || !is_cursor_text(value)) throw UtteranceCursorError("cursor is invalid");
    auto parsed = parse_json(str::sanitize_utf8(node_base64url_decode(value)), JsonLimits{MAX_CURSOR_LENGTH, 16, 4});
    if (!parsed || !parsed->is_array() || parsed->size() != 2) throw UtteranceCursorError("cursor is invalid");
    const Json& a = (*parsed)[0];
    const Json& b = (*parsed)[1];
    if (!a.is_number() || !b.is_string()) throw UtteranceCursorError("cursor is invalid");
    const double posted = a.get<double>();
    if (!std::isfinite(posted) || std::trunc(posted) != posted || posted > kMaxSafeInteger || posted < 0)
        throw UtteranceCursorError("cursor is invalid");
    const std::string id = b.get<std::string>();
    const auto len = str::js_length(id);
    if (len == 0 || len > MAX_UTTERANCE_ID_LENGTH) throw UtteranceCursorError("cursor is invalid");
    return {static_cast<std::int64_t>(posted), id};
}

std::optional<std::string> next_utterance_cursor(const std::vector<ExportableUtteranceRecord>& utterances) {
    if (utterances.empty()) return std::nullopt;
    const auto& last = utterances.back();
    const auto posted = time::parse_iso(last.createdAt);
    if (!posted || *posted < 0 || static_cast<double>(*posted) > kMaxSafeInteger)
        throw UtteranceCursorError("utterance createdAt is invalid");
    return encode_utterance_cursor({*posted, last.id});
}

std::vector<ExportableUtteranceRecord> read_human_utterances(sql::Database& db, const std::string& author_id,
                                                             const UtteranceCursor& after, std::int64_t limit) {
    const auto rows = db.all(R"(SELECT u.id, u.raw_content AS text, u.posted_at AS postedAt
       FROM utterances u
       JOIN sessions s
         ON s.id = u.session_id
        AND s.workspace_id = u.workspace_id
      WHERE u.speaker_id = ?
        AND s.title LIKE 'discord-session:%'
        AND s.scene LIKE 'discord:%'
        AND (u.posted_at > ? OR (u.posted_at = ? AND u.id > ?))
      ORDER BY u.posted_at ASC, u.id ASC
      LIMIT ?)",
                             {author_id, after.postedAt, after.postedAt, after.id, limit});
    std::vector<ExportableUtteranceRecord> out;
    out.reserve(rows.size());
    for (const auto& row : rows) out.push_back({row.str("id"), row.str("text"), iso_from_ms(row.num("postedAt"))});
    return out;
}

} // namespace di::persona_bridge
