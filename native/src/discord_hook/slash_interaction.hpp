#pragma once
// slash コマンドの interaction (Gateway の INTERACTION_CREATE の d) と、それへの返信口 (w3-M の局所の補助)。
//
// discord.js の ChatInputCommandInteraction の代わり。Gateway (受信) は後の担当なので、ここではハンドラが
//   - interaction の JSON から commandName / options を読む (SlashInteraction)
//   - 返信を InteractionResponder に渡す (REST 実装は RestInteractionResponder: DiscordApi の callback / @original 編集)
// の形をとる。テストでは InteractionResponder を差し替える。
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/json.hpp"
#include "discord_hook/diagnostic_reply.hpp"
#include "discord_hook/discord_api.hpp"

namespace di::discord_hook {

/** interaction の応答種別 (Discord API の InteractionResponseType)。 */
namespace interaction_response {
inline constexpr int ChannelMessageWithSource = 4;
inline constexpr int DeferredChannelMessageWithSource = 5;
} // namespace interaction_response
/** MessageFlags.Ephemeral */
inline constexpr int kEphemeralFlag = 64;

struct SlashOption {
    std::string name;
    int type = 0;
    Json value;
};

struct SlashInteraction {
    std::string id;
    std::string token;  // 秘密 (interaction token)。ログに出さない。
    std::string applicationId;
    std::string commandName;
    std::optional<std::string> guildId;
    std::optional<std::string> channelId;
    std::optional<std::string> userId;
    std::vector<SlashOption> options;

    /** interaction.options.getString(name) (無い・文字列でなければ nullopt)。 */
    std::optional<std::string> get_string(std::string_view name) const;
    std::optional<bool> get_boolean(std::string_view name) const;
};

/** INTERACTION_CREATE の d (type=2 APPLICATION_COMMAND) を読む。形が違えば std::runtime_error。 */
SlashInteraction slash_interaction_from_json(const Json& d);

class InteractionResponder {
public:
    virtual ~InteractionResponder() = default;
    /** interaction.deferReply({ flags: Ephemeral }) */
    virtual void defer_ephemeral() = 0;
    /** interaction.editReply(reply) */
    virtual void edit_reply(const DiagnosticReply& reply) = 0;
};

/** DiscordApi で返信する実装。失敗は DiscordApiError (呼び出し側へ伝える)。 */
class RestInteractionResponder : public InteractionResponder {
public:
    RestInteractionResponder(DiscordApi& api, SlashInteraction interaction) : api_(api), interaction_(std::move(interaction)) {}
    void defer_ephemeral() override;
    void edit_reply(const DiagnosticReply& reply) override;

private:
    DiscordApi& api_;
    SlashInteraction interaction_;
};

} // namespace di::discord_hook
