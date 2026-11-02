#pragma once
// 軽量 Web チャット UI — Discord を介さず議論に参加する経路 (src/api/web-chat-routes.ts の移植)。
//
// Di の議論は Discord Gateway / フォーラムが正路だが、ここでは同じ議論エンジン
// (分類器 → designGap → persona-engine → facilitator) を `scene=web:<roomId>` という
// トランスポートでそのまま再利用する。出力は外部投稿せず、core に永続した発話をブラウザがポーリングで取得する。
//
//   GET  /chat                         チャット UI (native/web/api/chat.html、loopback 前提)
//   GET  /api/chat/sessions            チャット議論一覧 (?state= &limit= &offset=)
//   GET  /api/chat/:room/messages      会話取得 (?since=<ms> で増分)
//   POST /api/chat/:room/messages      人間発話を投入 (進行中議論が無ければ designGap を起こす)
//
// 認証は他の HTTP UI と同じく loopback 信頼 (middleware/auth)。個人データは保存しない
// (author は表示名のみ、40 文字まで)。ルートは index で app 直下に mount する (TS と同じ絶対パス)。
#include <functional>
#include <memory>
#include <string>

#include "api/consensus_routes.hpp"
#include "discord_hook/auto_discussion.hpp"
#include "infra/http.hpp"
#include "web_chat/transcript.hpp"

namespace di::api {

/** 平文投稿から議題を自動検出して persona-engine の議論開始へつなぐ (gateway と同じ starter を注入)。 */
using ClassifyInboundMessage = std::function<discord_hook::StarterResult(const discord_hook::DiscordAutoDiscussionInput&)>;

struct WebChatDeps {
    std::string workspaceId = "knowledge";
    ClassifyInboundMessage classifyInboundMessage;
    /** persona id → 表示名 (peDb の personas から解決)。 */
    web_chat::SpeakerNameResolver resolveSpeakerName;
    /** active KG を開く (既定 open_active_core)。 */
    CoreOpener openCore;
};

/** 起動後に deps を差し替える (TS の setWebChatDeps)。スレッド安全。 */
void set_web_chat_deps(WebChatDeps deps);

/** POST 本文の上限 (本文 + 表示名だけなので 64 KiB)。 */
inline constexpr std::size_t kWebChatMaxBody = 64u << 10;

http::Router web_chat_routes();

} // namespace di::api
