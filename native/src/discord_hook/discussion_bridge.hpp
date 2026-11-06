#pragma once
// 議論 utterance / hypothesis を Discord channel に投稿するブリッジ (PR-I, src/discord-hook/discussion-bridge.ts の移植)。
//
// 流れ: persona-engine が post_utterance / propose_hypothesis → adapter の onAfterPost で本ブリッジが発火 →
// session.scene から "discord:<guildId>/<channelId>" を取り出し → workspace の discord monitor (無ければ config) の
// bot token で channel に投稿する。失敗は戻り値で返すだけ (= 議論を止めない)。token はログ・戻り値に出さない。
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "base/json.hpp"
#include "core/index.hpp"
#include "discord_hook/discord_api.hpp"

namespace di::discord_hook {

struct DiscussionPostArgs {
    core::Core* core = nullptr;
    std::string workspaceId;
    std::string sessionId;
    /** "utterance" | "hypothesis" */
    std::string kind = "utterance";
    /** 発話者 (persona name 等、表示用) */
    std::string speakerLabel;
    std::string text;
    /** true: persona を webhook で人間名 (speakerLabel) として投稿する。false: bot として直接投稿。 */
    bool viaWebhook = false;
};

struct DiscussionPostResult {
    bool ok = false;
    /** true: 失敗ではなく「意図的に投稿しなかった」(= headless 議論など非 discord scene)。caller は warn しない。 */
    bool skipped = false;
    std::optional<std::string> reason;
    std::optional<std::string> channelId;
    std::optional<std::string> messageId;
};
Json to_json(const DiscussionPostResult& r);

DiscussionPostResult post_discussion_to_discord(const DiscussionPostArgs& args);

/** bot token から DiscordApi を得る口 (既定は token ごとに 1 つ作って保持)。テストで偽サーバへ向けるのに使う。 */
using DiscordApiFactory = std::function<std::shared_ptr<DiscordApi>(const std::string& bot_token)>;
void set_discussion_bridge_api_factory(DiscordApiFactory factory);

} // namespace di::discord_hook
