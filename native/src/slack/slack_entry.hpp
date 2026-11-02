#pragma once
// Slack 議論入口の純粋ロジック — src/slack/slack-entry.ts の移植。
// Block Kit の組み立て、進行量プリセットの解析、message event がスレッド開始かの判定など。
// Slack の event/payload は形が緩いので JSON から安全に取り出す。
#include <optional>
#include <string>
#include <vector>

#include "base/json.hpp"
#include "slack/web_api.hpp"

namespace di::slack {

inline constexpr const char* SLACK_FLOW_PICK_ACTION = "slack-flow-pick";
inline constexpr const char* SLACK_FLOW_SETTINGS_ACTION = "slack-flow-settings";

/** 議論タイプ選択の Block Kit (section + static_select の actions)。 */
std::vector<SlackBlock> build_flow_pick_blocks();
/** 進行量プリセット選択の Block Kit。 */
std::vector<SlackBlock> build_flow_settings_blocks();

struct SettingsPreset {
    std::optional<double> rounds;
    std::optional<double> turnsPerRound;
};
/** "3x6" → {3, 6}。"default" / 不正は {}。Discord 版 parse_settings_preset と同仕様。 */
SettingsPreset parse_settings_preset(const std::string& value);

/** Slack message event が「スレッド開始 (トップレベル投稿)」か (subtype / bot_id 付きは除外)。 */
bool is_thread_starter(const Json& event);
/** block_actions payload の最初の選択値 (static_select の value)。 */
std::optional<std::string> first_selected_value(const Json& payload);
/** block_actions payload の最初の action_id。 */
std::optional<std::string> first_action_id(const Json& payload);

} // namespace di::slack
