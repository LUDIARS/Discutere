#pragma once
// 外部会話への参加 (DI-EXTERNAL-DISCUSSION) の入出力契約。移植元 src/external-discussion/contracts.ts。
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "base/json.hpp"

namespace di::external_discussion {

struct ConversationMessage {
    std::string id;
    std::string kind;  // "human" | "ai"
    std::string text;
    std::int64_t at = 0;
};

struct ConversationInput {
    std::string scene;
    bool enabled = false;
    std::vector<ConversationMessage> messages;
};

struct ParticipationResult {
    std::string status;  // "disabled" | "waiting" | "busy" | "skipped" | "proposal"
    std::optional<std::string> proposalId;
    std::optional<std::string> text;
    std::optional<std::string> stance;  // "pro" | "con"
    std::optional<std::string> sourceMessageId;
    bool operator==(const ParticipationResult&) const = default;
};

/** 入力の検査に失敗した (TS の throw new Error(...) と同じ文言)。 */
class ConversationError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/**
 * 検査して読み、messages を at 昇順 (同時刻は id の localeCompare 順) に並べる。
 * 失敗は ConversationError ("conversation required" / "invalid scene" / "invalid conversation" /
 * "invalid message" / "conversation too large")。
 */
ConversationInput parse_conversation(const Json& value);

Json to_json(const ConversationMessage& m);
Json to_json(const ConversationInput& input);
Json to_json(const ParticipationResult& r);
/** 保存済みの結果 JSON を読む (不明なキーは捨てる)。 */
ParticipationResult participation_result_from_json(const Json& j);

/**
 * String#localeCompare (ICU の root 照合) の近似。ASCII は ICU と同じ順 (記号 < 数字 < 英字、
 * 英字は大小を無視して比べ、同じなら小文字が先、制御文字は無視)。非 ASCII は ASCII の後ろに code point 順。
 */
int locale_compare(const std::string& a, const std::string& b);

} // namespace di::external_discussion
