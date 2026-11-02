#pragma once
// Discord application command (slash) 定義 (single source of truth, src/discord-hook/command-defs.ts の移植)。
//
// 起動時 (gateway ClientReady) と手動 script の双方から Discord REST `PUT /applications/{id}/commands` に流し込む。
// routeSlashCommand (command-router) が処理する name と必ず一致させること。
// JSON の形 (キー順を含む) は Node の DISCORD_COMMAND_DEFS を JSON.stringify したものと 1 バイトも違わない (golden)。
#include "base/json.hpp"

namespace di::discord_hook {

/** discord.js ApplicationCommandOptionType と同じ値。 */
namespace option_type {
inline constexpr int String = 3;
inline constexpr int Integer = 4;
inline constexpr int Boolean = 5;
} // namespace option_type

/** RESTPostAPIChatInputApplicationCommandsJSONBody[] */
const Json& DISCORD_COMMAND_DEFS();

} // namespace di::discord_hook
