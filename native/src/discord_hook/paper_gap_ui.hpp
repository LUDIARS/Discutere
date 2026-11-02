#pragma once
// ペーパーの不足情報 (paper gap) の modal UI — src/discord-hook/paper-gap-ui.ts の移植。
// modal は discord.js ModalBuilder の toJSON() と同じ Discord API の JSON で返す。
#include <optional>
#include <string>
#include <vector>

#include "base/json.hpp"

namespace di::discord_hook {

inline constexpr const char* PAPER_GAP_COMMAND_NAME = "discutere-paper-gap";
inline constexpr const char* PAPER_GAP_MODAL_PREFIX = "paper-gap";

struct PaperGapTypeDef {
    const char* value;
    const char* name;
    const char* description;
};
/** PAPER_GAP_TYPES (game_content / user_reaction / spec / mechanics / project_code / other)。 */
const std::vector<PaperGapTypeDef>& paper_gap_types();
/** slash の choices ([{name, value}])。 */
Json paper_gap_type_choices();

namespace paper_gap_field_ids {
inline constexpr const char* target = "target";
inline constexpr const char* detail = "detail";
inline constexpr const char* source = "source";
} // namespace paper_gap_field_ids

bool is_paper_gap_type(const std::string& value);
std::string label_for_paper_gap_type(const std::string& type);

/** customId `paper-gap:<type>:<channelId>` の modal (対象・見出し / 追加・修正したい内容 (必須) / 根拠)。 */
Json build_paper_gap_modal(const std::string& type, const std::string& channel_id);

struct PaperGapModalId {
    std::string type;
    std::string channelId;
};
std::optional<PaperGapModalId> parse_paper_gap_modal_custom_id(const std::string& custom_id);

struct PaperGapInstructionInput {
    std::string type;
    std::optional<std::string> target;
    std::string detail;
    std::optional<std::string> source;
};
std::string format_paper_gap_review_instruction(const PaperGapInstructionInput& input);

} // namespace di::discord_hook
