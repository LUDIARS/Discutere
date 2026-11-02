#pragma once
// 発話の投入口 (src/core/projection/message-input.ts の移植)。
// "/command args" ならコマンドとして処理し (utterance は作らない)、それ以外は utterance を 1 件記録する。
#include <optional>
#include <string>

#include "base/json.hpp"
#include "core/projection/types.hpp"

namespace di::core::projection {

struct SubmitMessageInput {
    ReturnTypeCreateCore* core = nullptr;
    std::string workspaceId;
    std::string sessionId;
    std::optional<std::string> personId;
    std::string rawContent;
};

/** { ok, message? , error? } (ok のとき message、!ok のとき error)。 */
struct CommandResult {
    bool ok = false;
    std::optional<std::string> message;
    std::optional<std::string> error;
};
/** TS と同じキー順 ({ok, message} / {ok, error})。 */
Json to_json(const CommandResult& r);

struct SubmitMessageResult {
    /** 記録した utterance の id (コマンドなら空)。 */
    std::string utteranceId;
    std::optional<CommandResult> commandResult;
};
Json to_json(const SubmitMessageResult& r);

SubmitMessageResult submit_message(const SubmitMessageInput& input);

} // namespace di::core::projection
