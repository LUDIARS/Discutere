#pragma once
// Slack Web API 薄クライアント — src/slack/web-api.ts の移植 (http::fetch 直叩き、@slack/* SDK なし)。
//
// 認証は 2 種類のトークンを使い分ける (どちらも Bearer。ログ・エラー文に出さない):
//   - app-level token (xapp-) : Socket Mode の wss URL 取得 (apps.connections.open)
//   - bot token (xoxb-)       : Web API (chat.postMessage / reactions.add)
#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "base/json.hpp"

namespace di::slack {

/** Slack Block Kit のブロック (各 builder が返す JSON)。 */
using SlackBlock = Json;

/** 取得先・期限・上限 (テストで偽サーバに差し替える)。 */
struct WebApiOptions {
    std::string base_url = "https://slack.com/api";
    std::chrono::milliseconds timeout{15'000};
    std::size_t max_response_bytes = 1u << 20;
    /** 偽サーバ (loopback) を使うテストだけ true。 */
    bool allow_private_network = false;
};

/** Slack 本文の上限ガード (UTF-16 で max 文字を超えたら末尾を … に置換)。 */
std::string truncate_slack(const std::string& s, std::size_t max = 3000);

/**
 * 共通 POST (Bearer + JSON)。応答 JSON を返す。読めない本文は {ok:false, error:"invalid_json"}。
 * 通信失敗は std::runtime_error ("slack <method>: <理由>")。
 */
Json slack_post(const std::string& method, const std::string& token, const Json& body, const WebApiOptions& options = {});

/** Socket Mode の WebSocket URL を取得する (apps.connections.open)。ok!==true なら std::runtime_error。 */
std::string apps_connections_open(const std::string& app_token, const WebApiOptions& options = {});

struct PostMessageArgs {
    std::string channel;
    std::string text;
    std::optional<std::string> threadTs;
    std::optional<std::string> username;
    std::optional<std::string> iconEmoji;
    std::optional<std::vector<SlackBlock>> blocks;
};

struct PostedMessage {
    std::string ts;
    std::string channel;
};

/** chat.postMessage。ok!==true なら std::runtime_error。 */
PostedMessage post_message(const std::string& bot_token, const PostMessageArgs& args, const WebApiOptions& options = {});

struct ReactionsAddArgs {
    std::string channel;
    std::string timestamp;
    /** 絵文字名 (例 "trophy" / "+1")。コロン無し。 */
    std::string name;
};

/** reactions.add。already_reacted は無視、それ以外の失敗は warn のみ。 */
void reactions_add(const std::string& bot_token, const ReactionsAddArgs& args, const WebApiOptions& options = {});

} // namespace di::slack
