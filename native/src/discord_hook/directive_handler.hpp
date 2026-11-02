#pragma once
// 進行役への調整指示の取り込み (transport 非依存, src/discord-hook/directive-handler.ts の移植)。
//
// gateway が bot メンションを検知したら、本文 (メンション除去済) と発話場所 (guild/channel) をここへ渡す。ここでは:
//   1. scene "discord:<guild>/<channel>" から進行中の議論 (design gap) を引く
//   2. 引けたら調整指示として gap に紐付けて保存する
//   3. Discord に返す「了解」文 (ack) を組み立てて返す
// core (KG) の open/close はここに閉じ込める (active KG を config から解決する)。
#include <optional>
#include <string>

#include "base/sqlite.hpp"

namespace di::discord_hook {

struct DirectiveMessageInput {
    std::string workspaceId;
    /** guild id (DM は "dm")。 */
    std::string guildId;
    /** 発話チャンネル id (フォーラムは thread id)。scene の解決に使う。 */
    std::string channelId;
    /** 調整指示の本文 (bot メンション除去済)。 */
    std::string text;
    /** 指示を出した Discord user id (監査用)。 */
    std::optional<std::string> authorId;
};

struct DirectiveMessageResult {
    /** 指示として保存できたか (false なら gap 不在 or 空文 で未保存)。 */
    bool stored = false;
    /** Discord に返す ack 文 (常に非空)。 */
    std::string reply;
    /** 保存先 gap id (stored=true のときのみ)。 */
    std::optional<std::string> gapId;
};

/** active KG を開いて取り込む。 */
DirectiveMessageResult handle_directive_message(const DirectiveMessageInput& input);
/** 開いた KG (core の SQLite) に取り込む版 (テスト・既に開いている呼び出し側用)。 */
DirectiveMessageResult handle_directive_message(sql::Database& kg, const DirectiveMessageInput& input);

} // namespace di::discord_hook
