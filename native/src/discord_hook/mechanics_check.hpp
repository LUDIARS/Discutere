#pragma once
// Dedicated one-reply adapter; never enters the generic discussion router (src/discord-hook/mechanics-check.ts の移植)。
#include <chrono>
#include <optional>
#include <string>

#include "discord_hook/diagnostic_reply.hpp"
#include "discord_hook/slash_interaction.hpp"
#include "mechanics_check/contracts.hpp"
#include "mechanics_check/tool.hpp"

namespace di::discord_hook {

inline constexpr const char* MECHANICS_CHECK_COMMAND_NAME = "mechanics-check";

/** Numeric-model input is parsed but explicitly rejected by the mechanics boundary. 失敗は MechanicsCheckError("invalid_input" など)。 */
mechanics_check::MechanicsCheckRequest read_mechanics_check_slash_input(const std::string& spec_text, const std::optional<std::string>& baseline_text,
                                  const std::optional<std::string>& input_text);

/** JSON content is bounded in bytes and mentions are disabled for both delivery modes. */
DiagnosticReply mechanics_check_reply(const mechanics_check::MechanicsCheckResult& result);

/**
 * commandName が "mechanics-check" でなければ false (何もしない)。そうなら deferReply(ephemeral) → 診断 → editReply を
 * 1 回ずつ行って true。診断の失敗は "診断失敗 (...)" の返信にする (LLM のエラー内容は出さない)。
 * deferReply / editReply の失敗 (DiscordApiError 等) は呼び出し側へ投げる (defer 失敗時は LLM を呼ばない)。
 * deps が nullptr (LLM 未設定) なら llm_unavailable。
 */
bool handle_mechanics_check_command(const SlashInteraction& interaction, InteractionResponder& responder, const mechanics_check::MechanicsCheckDependencies* deps,
                          std::optional<std::chrono::milliseconds> timeout = std::nullopt);

} // namespace di::discord_hook
