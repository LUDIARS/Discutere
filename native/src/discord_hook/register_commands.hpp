#pragma once
// Discord application command の登録 (REST PUT, src/discord-hook/register-commands.ts の移植)。
//
// - guildIds 指定あり: 各 guild に guild command として登録 (反映が即時)。"dm" と空文字は除く。
// - guildIds 空: global command として登録 (反映に最大 1 時間)。
// 起動時 (gateway ClientReady) と手動 script の双方から呼べる。token は引数で受け取り、ログには出さない。
#include <string>
#include <vector>

#include "base/json.hpp"
#include "discord_hook/discord_api.hpp"

namespace di::discord_hook {

struct RegisterCommandsArgs {
    std::string botToken;
    /** application id。bot の場合 client.application.id と一致する */
    std::string applicationId;
    /** 登録先 guild 群。空なら global 登録 */
    std::vector<std::string> guildIds;
};

struct RegisterCommandsResult {
    /** "guild" | "global" */
    std::string scope;
    std::vector<std::string> guildIds;
    std::size_t commandCount = 0;
};
Json to_json(const RegisterCommandsResult& r);

/**
 * DISCORD_COMMAND_DEFS を PUT する。botToken / applicationId が空なら std::invalid_argument
 * ("registerSlashCommands: botToken required" 等)。REST の失敗は DiscordApiError。
 * api_options は base_url 等の差し替え用 (token は args.botToken で上書きする)。
 */
RegisterCommandsResult register_slash_commands(const RegisterCommandsArgs& args, DiscordApiOptions api_options = {});
/** 既に作った DiscordApi で登録する版。 */
RegisterCommandsResult register_slash_commands(DiscordApi& api, const std::string& application_id, const std::vector<std::string>& guild_ids);

} // namespace di::discord_hook
