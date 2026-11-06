#pragma once
// Discord channel への message 投稿 (src/discord-hook/poster.ts の移植)。
//
// - Bot Token で `POST /channels/{id}/messages` (DiscordApi 経由)。失敗は DiscordApiError (caller が catch & log)。
// - persona を人間名で投稿するための webhook (チャンネルごとに 1 つ確保してプロセス内で保持)。
#include <optional>
#include <string>

#include "discord_hook/discord_api.hpp"

namespace di::discord_hook {

struct DiscordPostArgs {
    std::string channelId;
    std::string content;
    /** 返信元 message id (任意) */
    std::optional<std::string> replyTo;
};

/** 本文は 1900 文字 (UTF-16) で切る。投稿した message id を返す。 */
std::string post_discord_channel(DiscordApi& api, const DiscordPostArgs& args);

/** bot 名義で message に絵文字リアクションを付ける (URL エンコードして送る)。失敗は例外。 */
void react_discord(DiscordApi& api, const std::string& channel_id, const std::string& message_id, const std::string& emoji);

struct WebhookCredentials {
    std::string id;
    /** webhook の token (秘密。ログに出さない)。 */
    std::string token;
};

/** channel に議論用 webhook ("Discutere議論") を確保 (なければ作成)。Manage Webhooks 権限が要る。 */
WebhookCredentials ensure_channel_webhook(DiscordApi& api, const std::string& channel_id);
/** テスト/再接続用: webhook の保持を消す。 */
void clear_webhook_cache();

struct WebhookTarget {
    std::string webhookChannelId;
    std::optional<std::string> threadId;
};
/** スレッドは webhook を持てないので親チャンネルの webhook + thread_id で流す。失敗時は channelId をそのまま。 */
WebhookTarget resolve_webhook_target(DiscordApi& api, const std::string& channel_id);

struct DiscordWebhookPostArgs {
    std::string webhookId;
    std::string webhookToken;
    std::string username;
    std::string content;
    std::optional<std::string> avatarUrl;
    std::optional<std::string> threadId;
};

/** webhook で username (= persona の人間名) を変えて投稿。message id を返す (応答が読めなければ "")。 */
std::string post_discord_webhook(DiscordApi& api, const DiscordWebhookPostArgs& args);

/** 先頭の機械的ラベル (反証:/反例: 等) を除き、文末 (。！？!?) ごとに改行する。 */
std::string humanize_for_discord(const std::string& text);

} // namespace di::discord_hook
