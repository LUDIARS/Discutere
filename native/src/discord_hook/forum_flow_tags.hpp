#pragma once
// フォーラム議論タグ (新フロー入口 — T7 経路 B) — src/discord-hook/forum-flow-tags.ts の移植。
//
// Discord フォーラムを「議論の UI」として使うため、Di が必要とする定義済みタグを用意する:
//   - 議論タイプ (必須・1 つ選択): 議論 / 改善
//   - 機密度+観点 (任意・複数可): 機密 / 内部 / 運用 / 開発 / 未来予測 / 代理
// 起動時に議論フォーラムを ensure し、不足タグを補充する (REST: DiscordApi)。
// UI (select / button / modal) は discord.js の builder の代わりに Discord API の component JSON を返す
// (builder の toJSON() と同じ形)。
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/json.hpp"
#include "discord_hook/discord_api.hpp"

namespace di::discord_hook {

/** 議論タイプ (必須・1 つ選択) のフォーラムタグ名。順序は select UI の表示順。 */
const std::vector<std::string>& flow_kind_tag_names();
/** 機密度+観点 (任意・複数可) のフォーラムタグ名。FlowTag と一致。 */
const std::vector<std::string>& flow_aspect_tag_names();
/** Di が議論フォーラムに用意する全タグ名。 */
const std::vector<std::string>& all_flow_tag_names();

inline constexpr const char* FLOW_PICK_PREFIX = "flow-pick";
inline constexpr const char* FLOW_SETTINGS_PREFIX = "flow-settings";
inline constexpr const char* FLOW_CUSTOM_BTN_PREFIX = "flow-custom";
inline constexpr const char* FLOW_MODAL_PREFIX = "flow-modal";

/** 議論タイプ select メニューの action row (customId `flow-pick:<threadId>`)。 */
Json build_flow_pick_menu(const std::string& thread_id);
std::optional<std::string> parse_flow_pick_custom_id(const std::string& custom_id);

/** 進行量設定メッセージの components ([select の row, 「数値を指定」ボタンの row])。 */
std::vector<Json> build_flow_settings_components(const std::string& thread_id);
/** ラウンド/ターン数を入力するモーダル。 */
Json build_flow_settings_modal(const std::string& thread_id);

std::optional<std::string> parse_flow_settings_custom_id(const std::string& id);
std::optional<std::string> parse_flow_custom_btn_custom_id(const std::string& id);
std::optional<std::string> parse_flow_modal_custom_id(const std::string& id);

struct SettingsPreset {
    std::optional<double> rounds;
    std::optional<double> turnsPerRound;
};
/** "3x6" → {3, 6}。"default" / 不正は {} (= config 既定)。/^(\d+)x(\d+)$/ と同じ。 */
SettingsPreset parse_settings_preset(const std::string& value);

struct MergedForumTags {
    std::vector<ForumTag> tags;
    std::vector<std::string> added;
};
/**
 * 既存タグに不足分を足したタグ配列を返す (純関数)。上限 20。既存タグは温存 (壁打ち/学習 系の
 * 廃止タグだけは除く) し、want の未登録分を末尾に追加する。上限を超える分は追加しない。
 */
MergedForumTags merge_forum_tags(const std::vector<ForumTag>& existing, const std::vector<std::string>& want = all_flow_tag_names());

/** フォーラムの available_tags に Di の定義済みタグを補充する (変化が無ければ API を叩かない)。足した名前を返す。 */
std::vector<std::string> ensure_forum_flow_tags(DiscordApi& api, const Channel& forum);

/**
 * guild に議論フォーラムを ensure する。同名の GuildForum があれば流用してタグ補充、無ければ全タグ付きで作成。
 * 権限不足・作成失敗は nullopt (caller が skip)。
 */
std::optional<Channel> ensure_discussion_forum(DiscordApi& api, const std::string& guild_id, const std::string& forum_name);

} // namespace di::discord_hook
