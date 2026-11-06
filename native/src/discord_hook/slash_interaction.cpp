#include "discord_hook/slash_interaction.hpp"

#include <stdexcept>

namespace di::discord_hook {

std::optional<std::string> SlashInteraction::get_string(std::string_view name) const {
    for (const auto& o : options)
        if (o.name == name) return o.value.is_string() ? std::optional<std::string>(o.value.get<std::string>()) : std::nullopt;
    return std::nullopt;
}

std::optional<bool> SlashInteraction::get_boolean(std::string_view name) const {
    for (const auto& o : options)
        if (o.name == name) return o.value.is_boolean() ? std::optional<bool>(o.value.get<bool>()) : std::nullopt;
    return std::nullopt;
}

SlashInteraction slash_interaction_from_json(const Json& d) {
    if (!d.is_object()) throw std::runtime_error("interaction: not an object");
    const auto& data = json_object(d, "data");
    SlashInteraction s;
    s.id = json_string(d, "id");
    s.token = json_string(d, "token");
    s.applicationId = json_string(d, "application_id");
    s.commandName = json_string(data, "name");
    if (s.id.empty() || s.token.empty() || s.commandName.empty()) throw std::runtime_error("interaction: missing id/token/name");
    s.guildId = json_opt_string(d, "guild_id");
    s.channelId = json_opt_string(d, "channel_id");
    // guild では member.user、DM では user。
    const auto& member = json_object(d, "member");
    const auto& user = member.is_object() && member.contains("user") ? json_object(member, "user") : json_object(d, "user");
    s.userId = json_opt_string(user, "id");
    for (const auto& o : json_array(data, "options")) {
        if (!o.is_object()) continue;
        SlashOption opt;
        opt.name = json_string(o, "name");
        opt.type = static_cast<int>(json_int(o, "type"));
        if (o.contains("value")) opt.value = o["value"];
        s.options.push_back(std::move(opt));
    }
    return s;
}

void RestInteractionResponder::defer_ephemeral() {
    api_.create_interaction_response(interaction_.id, interaction_.token,
                                     Json{{"type", interaction_response::DeferredChannelMessageWithSource},
                                          {"data", Json{{"flags", kEphemeralFlag}}}});
}

void RestInteractionResponder::edit_reply(const DiagnosticReply& reply) {
    std::vector<FileUpload> files;
    if (reply.file) files.push_back(*reply.file);
    api_.edit_original_interaction_response(interaction_.applicationId, interaction_.token, reply.body(), files);
}

} // namespace di::discord_hook
