#pragma once
// src/web-chat/transcript.ts の移植: Web チャット room の会話読み出し。
//
// 1 つの room (scene=web:<roomId>) には 2 種類の session がぶら下がる:
//   - 入口 session (`web-session:<roomId>`)      … 人間の平文発話
//   - 議論 session (`discussion-of-gap:<gapId>`) … persona / facilitator の発話 (+ ミラーした人間発話)
// 人間発話は入口と議論の両方に載る (ミラー) ため、人間は入口由来だけ、非人間は議論由来だけを採る。
// 個人アンカーは保存していない (匿名 workspace)。speaker_id の prefix で役割を判定する。
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "base/json.hpp"
#include "base/sqlite.hpp"
#include "core/index.hpp"

namespace di::web_chat {

/** "human" | "persona" | "external" | "facilitator" */
using ChatRole = std::string;

struct ChatMessage {
    std::string id;
    ChatRole role;
    /** 表示名 (persona は display_name 解決済 / 人間は author / 外部は「外部の声」)。 */
    std::string speaker;
    std::string content;
    Json postedAt;  // DB の値 (JS の number)
};
Json to_json(const ChatMessage& m);

/** persona id → 表示名を解決する (未解決なら id をそのまま)。 */
using SpeakerNameResolver = std::function<std::string(const std::string& persona_id)>;

struct ReadTranscriptOptions {
    /** この時刻 (ms) 以降の発話だけ返す (既定 0 = 全件)。 */
    double sinceTs = 0;
    /** 返す最大件数 (既定 500)。 */
    std::int64_t limit = 500;
    SpeakerNameResolver resolveName;
};

/** 1 回に読む行・バイトの上限 (DB の値も外部入力として課金する)。 */
inline constexpr sql::QueryLimits kTranscriptReadLimits{100'000, 64u << 20};

/** scene=web:<roomId> に属する全 session の発話を時系列 (古い順) で 1 本の会話として返す。 */
std::vector<ChatMessage> read_room_transcript(sql::Database& raw, const std::string& workspace_id, const std::string& room_id,
                                              const ReadTranscriptOptions& options = {});
inline std::vector<ChatMessage> read_room_transcript(core::Core& core, const std::string& workspace_id, const std::string& room_id,
                                                     const ReadTranscriptOptions& options = {}) {
    return read_room_transcript(core.raw(), workspace_id, room_id, options);
}

} // namespace di::web_chat
