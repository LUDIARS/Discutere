#pragma once
// 進行役への調整指示 (facilitator directive) のストア + 整形 (src/discord-hook/facilitator-directives.ts の移植)。
//
// Discord で bot に @メンションして「もう少し簡単な言葉で」のような進行の調整指示を出せるようにする。
// 指示は対象議論 (design gap) に紐付けて KG (SQLite) のサイドカー表 facilitator_directives に保存し、
// facilitator / persona の prompt に注入する。author_id は監査用に保存するが prompt には出さない。
#include <optional>
#include <string>
#include <vector>

#include "base/sqlite.hpp"

namespace di::discord_hook {

/** prompt に載せる最新指示の最大件数。 */
inline constexpr int DIRECTIVE_PROMPT_LIMIT = 5;
/** 1 件の指示本文の最大長 (UTF-16 単位。prompt トークン肥大 + 悪用入力の防御)。 */
inline constexpr std::size_t DIRECTIVE_TEXT_CAP = 300;

struct FacilitatorDirectiveInput {
    std::string workspaceId;
    std::string gapId;
    /** 調整指示の本文 (bot メンション除去済を渡す想定)。 */
    std::string text;
    /** 指示を出した Discord user id (監査用、prompt には出さない)。 */
    std::optional<std::string> authorId;
};

void ensure_facilitator_directive_table(sql::Database& db);
/** 保存した行の id (空指示なら nullopt)。 */
std::optional<std::string> add_facilitator_directive(sql::Database& db, const FacilitatorDirectiveInput& input);
/** 指定 gap の直近 limit 件を時系列 (古い→新しい) で返す。 */
std::vector<std::string> list_facilitator_directives(sql::Database& db, const std::string& workspace_id, const std::string& gap_id,
                                                     int limit = DIRECTIVE_PROMPT_LIMIT);
/** scene ("discord:<guild>/<channel>") の進行中議論 (open な gap) の id。無ければ nullopt。 */
std::optional<std::string> resolve_active_gap_for_scene(sql::Database& db, const std::string& workspace_id, const std::string& scene);
/** prompt に注入する調整指示ブロック (無ければ "")。 */
std::string format_directives_block(const std::vector<std::string>& directives);
/** bot メンション (<@id> / <@!id>) を除去し、空白を詰めて trim する (純関数)。 */
std::string strip_bot_mention(const std::string& content, const std::optional<std::string>& bot_id);
/** trim + 上限切り詰め。空なら nullopt。 */
std::optional<std::string> normalize_directive_text(const std::string& text);

} // namespace di::discord_hook
