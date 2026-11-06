#include "discord_hook/register_commands.hpp"

#include <stdexcept>

#include "discord_hook/command_defs.hpp"

namespace di::discord_hook {

Json to_json(const RegisterCommandsResult& r) {
    return Json{{"scope", r.scope}, {"guildIds", r.guildIds}, {"commandCount", r.commandCount}};
}

RegisterCommandsResult register_slash_commands(DiscordApi& api, const std::string& application_id, const std::vector<std::string>& guild_ids) {
    if (application_id.empty()) throw std::invalid_argument("registerSlashCommands: applicationId required");
    const auto& body = DISCORD_COMMAND_DEFS();
    std::vector<std::string> guilds;
    for (const auto& g : guild_ids)
        if (!g.empty() && g != "dm") guilds.push_back(g);
    if (!guilds.empty()) {
        for (const auto& guild_id : guilds) api.bulk_overwrite_commands(application_id, guild_id, body);
        return {"guild", guilds, body.size()};
    }
    api.bulk_overwrite_commands(application_id, std::nullopt, body);
    return {"global", {}, body.size()};
}

RegisterCommandsResult register_slash_commands(const RegisterCommandsArgs& args, DiscordApiOptions api_options) {
    if (args.botToken.empty()) throw std::invalid_argument("registerSlashCommands: botToken required");
    if (args.applicationId.empty()) throw std::invalid_argument("registerSlashCommands: applicationId required");
    api_options.token = args.botToken;
    DiscordApi api(std::move(api_options));
    return register_slash_commands(api, args.applicationId, args.guildIds);
}

} // namespace di::discord_hook
