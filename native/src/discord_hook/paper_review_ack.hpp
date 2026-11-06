#pragma once
// ペーパーレビュー中の回答を受け取ったことをリアクションで示す (src/discord-hook/paper-review-ack.ts の移植)。
// ✅ は「ペーパー承認 (議論開始)」の操作に使っているので、確認済みの印には ☑️ を使う。
// Discord のリプライ (message_reference つき) は対象外。
#include <functional>
#include <string>

#include "discord_hook/discord_api.hpp"

namespace di::discord_hook {

/** 確認済みの印。 */
inline constexpr const char* PAPER_REVIEW_ACK_EMOJI = "☑️";

/** リアクションを付けられる返信 (discord.js Message の必要部分)。react は失敗で例外を投げてよい。 */
struct AckableMessage {
    std::string content;
    bool has_reference = false;
    std::function<void(const std::string& emoji)> react;
};

/** Discord の Message から作る (react は DiscordApi::create_reaction)。 */
AckableMessage ackable_message(DiscordApi& api, const Message& msg);

/**
 * レビュー中スレッドへの本文ありの返信に確認済みリアクションを付ける。
 * 付与失敗 (権限不足など) は warn だけで、返信処理は止めない。リアクションを試みたら true。
 */
bool ack_paper_review_reply(const AckableMessage& msg, bool in_review,
                            const std::function<void(const std::string&)>& warn = {});

} // namespace di::discord_hook
