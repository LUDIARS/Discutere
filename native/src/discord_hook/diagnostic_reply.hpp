#pragma once
// Bounded UTF-8 delivery and safe errors shared by the two diagnostic adapters (src/discord-hook/diagnostic-reply.ts)。
#include <optional>
#include <string>

#include "base/json.hpp"
#include "discord_hook/discord_api.hpp"

namespace di::discord_hook {

/** interaction.editReply に渡す内容 (content + 添付 + allowedMentions={parse:[]})。 */
struct DiagnosticReply {
    std::string content;
    std::optional<FileUpload> file;
    /** edit_original_interaction_response に渡す本文 ({content, allowed_mentions:{parse:[]}})。 */
    Json body() const;
};

/**
 * 結果 JSON ({logic_differences, design_gap}) を返信にする。JSON.stringify(result, null, 2) が
 * 1850 文字 (UTF-16) 以下ならコードブロック、超えれば添付 (filename)。resultBytes を超えたら
 * DiagnosticError("result_too_large")。filename は "mechanics-check.json" | "level-check.json"。
 */
DiagnosticReply diagnostic_reply(const Json& result, const std::string& filename);

/** "診断失敗 (<code>): <理由>" (DiagnosticError 以外は llm_failed)。 */
std::string diagnostic_failure(const std::exception* error);

} // namespace di::discord_hook
