#include "discord_hook/level_check.hpp"

#include "base/json.hpp"
#include "base/strings.hpp"
#include "design_diagnostic/validation.hpp"
#include "level_check/request.hpp"

namespace di::discord_hook {

namespace {
/** inline JSON は discordInputChars (UTF-16) 以下なので、バイトでも 4 倍あれば足りる。 */
JsonLimits slash_input_limits() {
    JsonLimits l;
    l.max_bytes = level_check::LEVEL_CHECK_LIMITS.discordInputChars * 4;
    l.max_elements = 20'000;
    l.max_depth = 64;
    return l;
}
} // namespace

level_check::LevelCheckRequest read_level_check_slash_input(const std::string& main_text, const std::optional<std::string>& baseline_text,
                                  const std::optional<std::string>& input_text) {
    Json request = Json::object();
    request["levelText"] = main_text;
    if (baseline_text) request["baselineText"] = *baseline_text;
    if (input_text) {
        if (str::js_length(*input_text) > level_check::LEVEL_CHECK_LIMITS.discordInputChars)
            throw level_check::LevelCheckError("invalid_input", "input exceeds Discord JSON limit");
        const auto raw = parse_json(*input_text, slash_input_limits());
        if (!raw) throw level_check::LevelCheckError("invalid_input", "input must be valid JSON");
        static const std::vector<std::string> keys{"references", "playModel"};
        const Json extra = design_diagnostic::object(*raw, "input", keys, "invalid_input");
        for (auto it = extra.begin(); it != extra.end(); ++it) request[it.key()] = it.value();
    }
    return level_check::validate_level_check_request(request);
}

DiagnosticReply level_check_reply(const level_check::LevelCheckResult& result) { return diagnostic_reply(level_check::to_json(result), "level-check.json"); }

bool handle_level_check_command(const SlashInteraction& interaction, InteractionResponder& responder, const level_check::LevelCheckDependencies* deps,
                          std::optional<std::chrono::milliseconds> timeout) {
    if (interaction.commandName != LEVEL_CHECK_COMMAND_NAME) return false;
    responder.defer_ephemeral();
    DiagnosticReply reply;
    try {
        if (!deps || !deps->llm) throw level_check::LevelCheckError("llm_unavailable", "LLM not configured");
        const auto request = read_level_check_slash_input(interaction.get_string("level").value_or(""), interaction.get_string("baseline"),
                                                     interaction.get_string("input"));
        reply = level_check_reply(level_check::check_level_consistency(request, *deps, timeout));
    } catch (const std::exception& error) {
        reply = DiagnosticReply{diagnostic_failure(&error), std::nullopt};
    } catch (...) {
        reply = DiagnosticReply{diagnostic_failure(nullptr), std::nullopt};
    }
    responder.edit_reply(reply);
    return true;
}

} // namespace di::discord_hook
