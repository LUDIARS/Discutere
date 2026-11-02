#pragma once
// src/flow/persona-display.ts の移植: 発話主体の表示名 composer (OVERVIEW §名前表示)。
//   - 憑依なし: `陽介 (意見屋)`
//   - 憑依あり: `陽介 (意見屋/論者#a1b2c3)`
// トランスポート非依存の純関数。Discord/Slack/WebUI すべてここを通す。
#include <optional>
#include <string>

#include "flow/personas.hpp"

namespace di::flow {

/** スタンス → 表示ロール名 (未知のスタンスは空文字)。 */
std::string stance_label(const FlowStance& stance);
/** role → 表示ロール名 (未知のロールは空文字)。 */
std::string role_label(const FlowRole& role);

struct DisplayNameInput {
    std::string name;
    std::optional<FlowStance> stance;
    std::optional<FlowRole> role;
    std::optional<std::string> possessionName;
};

/** 「名前 (ロール名/憑依ペルソナ)」。ロール名はスタンス優先、無ければ role、両方無ければ素の名前のみ。 */
std::string compose_display_name(const DisplayNameInput& input);

} // namespace di::flow
