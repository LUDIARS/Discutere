#pragma once
// persona bridge への本人の発話の書き出し。移植元 src/persona-bridge/utterance-export.ts。
// Discord 由来 (sessions.title が discord-session: で scene が discord:) の本人の発話だけを返す。
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "base/sqlite.hpp"

namespace di::persona_bridge {

struct ExportedUtterance {
    std::string text;
    std::string createdAt;
};

/** DB の並び順のメタデータ (id) を含む。応答本文には id を出さない。 */
struct ExportableUtteranceRecord {
    std::string id;
    std::string text;
    std::string createdAt;
};

struct UtteranceCursor {
    std::int64_t postedAt = 0;
    std::string id;
};

/** TS の TypeError (カーソル・createdAt の不正)。 */
class UtteranceCursorError : public std::invalid_argument {
public:
    using std::invalid_argument::invalid_argument;
};

Json to_json(const ExportedUtterance& u);
Json to_json(const ExportableUtteranceRecord& u);

std::string encode_utterance_cursor(const UtteranceCursor& cursor);
/** 不正は UtteranceCursorError("cursor is invalid")。 */
UtteranceCursor decode_utterance_cursor(const std::string& value);
/** 最後の発話のカーソル (無ければ nullopt)。createdAt が読めなければ UtteranceCursorError。 */
std::optional<std::string> next_utterance_cursor(const std::vector<ExportableUtteranceRecord>& utterances);

std::vector<ExportableUtteranceRecord> read_human_utterances(sql::Database& db, const std::string& author_id,
                                                             const UtteranceCursor& after, std::int64_t limit);

} // namespace di::persona_bridge
