#include "discord_hook/mechanics_check.hpp"

#include "base/json.hpp"
#include "base/strings.hpp"
#include "design_diagnostic/validation.hpp"
#include "mechanics_check/request.hpp"

namespace di::discord_hook {

namespace {
/** inline JSON は discordInputChars (UTF-16) 以下なので、バイトでも 4 倍あれば足りる。 */
JsonLimits slash_input_limits() {
    JsonLimits l;
    l.max_bytes = mechanics_check::MECHANICS_CHECK_LIMITS.discordInputChars * 4;
    l.max_elements = 20'000;
    l.max_depth = 64;
    return l;
}
} // namespace

mechanics_check::MechanicsCheckRequest read_mechanics_check_slash_input(const std::string& main_text, const std::optional<std::string>& baseline_text,
                                  const std::optional<std::string>& input_text) {
    Json request = Json::object();
    request["specText"] = main_text;
    if (baseline_text) request["baselineText"] = *baseline_text;
    if (input_text) {
        if (str::js_length(*input_text) > mechanics_check::MECHANICS_CHECK_LIMITS.discordInputChars)
            throw mechanics_check::MechanicsCheckError("invalid_input", "input exceeds Discord JSON limit");
        const auto raw = parse_json(*input_text, slash_input_limits());
        if (!raw) throw mechanics_check::MechanicsCheckError("invalid_input", "input must be valid JSON");
        static const std::vector<std::string> keys{"references", "playModel"};
        const Json extra = design_diagnostic::object(*raw, "input", keys, "invalid_input");
        for (auto it = extra.begin(); it != extra.end(); ++it) request[it.key()] = it.value();
    }
    return mechanics_check::validate_mechanics_check_request(request);
}

DiagnosticReply mechanics_check_reply(const mechanics_check::MechanicsCheckResult& result) { return diagnostic_reply(design_diagnostic::to_json(result), "mechanics-check.json"); }

bool handle_mechanics_check_command(const SlashInteraction& interaction, InteractionResponder& responder, const mechanics_check::MechanicsCheckDependencies* deps,
                          std::optional<std::chrono::milliseconds> timeout) {
    if (interaction.commandName != MECHANICS_CHECK_COMMAND_NAME) return false;
    responder.defer_ephemeral();
    DiagnosticReply reply;
    try {
        if (!deps || !deps->llm) throw mechanics_check::MechanicsCheckError("llm_unavailable", "LLM not configured");
        const auto request = read_mechanics_check_slash_input(interaction.get_string("spec").value_or(""), interaction.get_string("baseline"),
                                                     interaction.get_string("input"));
        reply = mechanics_check_reply(mechanics_check::check_mechanics_consistency(request, *deps, timeout));
    } catch (const std::exception& error) {
        reply = DiagnosticReply{diagnostic_failure(&error), std::nullopt};
    } catch (...) {
        reply = DiagnosticReply{diagnostic_failure(nullptr), std::nullopt};
    }
    responder.edit_reply(reply);
    return true;
}

} // namespace di::discord_hook
