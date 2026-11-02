#include "discord_hook/forum_flow_tags.hpp"

#include <set>

#include "base/log.hpp"
#include "base/strings.hpp"
#include "flow/project_premise.hpp"

namespace di::discord_hook {

const std::vector<std::string>& flow_kind_tag_names() {
    static const std::vector<std::string> v{"議論", "改善"};
    return v;
}

const std::vector<std::string>& flow_aspect_tag_names() {
    static const std::vector<std::string> v{"機密", "内部", "運用", "開発", "未来予測", "代理"};
    return v;
}

const std::vector<std::string>& all_flow_tag_names() {
    static const std::vector<std::string> v = [] {
        std::vector<std::string> out = flow_kind_tag_names();
        for (const auto& n : flow_aspect_tag_names()) out.push_back(n);
        return out;
    }();
    return v;
}

namespace {

Json select_option(const std::string& value, const std::string& label, const std::string& description) {
    return Json{{"value", value}, {"label", label}, {"description", description}};
}

Json action_row(Json component) { return Json{{"type", 1}, {"components", Json::array({std::move(component)})}}; }

Json string_select(const std::string& custom_id, const std::string& placeholder, Json options) {
    return Json{{"type", 3}, {"custom_id", custom_id}, {"placeholder", placeholder}, {"min_values", 1}, {"max_values", 1}, {"options", std::move(options)}};
}

Json text_input(const std::string& custom_id, const std::string& label, const std::string& placeholder, bool required, int style) {
    return Json{{"type", 4}, {"custom_id", custom_id}, {"label", label}, {"placeholder", placeholder}, {"required", required}, {"style", style}};
}

std::optional<std::string> parse_prefixed(const std::string& prefix, const std::string& custom_id) {
    if (!str::starts_with(custom_id, prefix + ":")) return std::nullopt;
    std::string id = custom_id.substr(prefix.size() + 1);
    if (id.empty()) return std::nullopt;
    return id;
}

} // namespace

Json build_flow_pick_menu(const std::string& thread_id) {
    const auto& p = flow::project_premises();
    Json options = Json::array({select_option("議論", p.discussion.label, p.discussion.description),
                                select_option("改善", p.improvement.label, p.improvement.description)});
    return action_row(string_select(std::string(FLOW_PICK_PREFIX) + ":" + thread_id, "議論タイプを選択 (必須)", std::move(options)));
}

std::optional<std::string> parse_flow_pick_custom_id(const std::string& custom_id) { return parse_prefixed(FLOW_PICK_PREFIX, custom_id); }

std::vector<Json> build_flow_settings_components(const std::string& thread_id) {
    Json options = Json::array({
        select_option("default", "標準で開始", "設定ファイルの既定ラウンド/ターン数で開始"),
        select_option("2x4", "さくっと (2R×4T)", "短時間・低コスト"),
        select_option("3x6", "標準量 (3R×6T)", "バランス型"),
        select_option("5x8", "じっくり (5R×8T)", "論点を深掘り"),
        select_option("8x12", "とことん (8R×12T)", "大ボリューム (高コスト)"),
    });
    Json menu = string_select(std::string(FLOW_SETTINGS_PREFIX) + ":" + thread_id, "進行量を選んで開始 (ラウンド数 × ターン数)", std::move(options));
    Json button{{"type", 2},
                {"emoji", Json{{"name", "⚙️"}, {"animated", false}}},
                {"custom_id", std::string(FLOW_CUSTOM_BTN_PREFIX) + ":" + thread_id},
                {"label", "数値を指定して開始"},
                {"style", 2}};  // ButtonStyle.Secondary
    return {action_row(std::move(menu)), action_row(std::move(button))};
}

Json build_flow_settings_modal(const std::string& thread_id) {
    return Json{{"custom_id", std::string(FLOW_MODAL_PREFIX) + ":" + thread_id},
                {"title", "進行量を指定"},
                {"components", Json::array({action_row(text_input("rounds", "ラウンド数 (1〜10)", "例: 3", false, 1)),
                                            action_row(text_input("turns", "1ラウンドのターン数 (1〜20)", "例: 6", false, 1))})}};
}

std::optional<std::string> parse_flow_settings_custom_id(const std::string& id) { return parse_prefixed(FLOW_SETTINGS_PREFIX, id); }
std::optional<std::string> parse_flow_custom_btn_custom_id(const std::string& id) { return parse_prefixed(FLOW_CUSTOM_BTN_PREFIX, id); }
std::optional<std::string> parse_flow_modal_custom_id(const std::string& id) { return parse_prefixed(FLOW_MODAL_PREFIX, id); }

SettingsPreset parse_settings_preset(const std::string& value) {
    const auto x = value.find('x');
    auto digits = [](std::string_view s) {
        if (s.empty()) return false;
        for (char c : s)
            if (c < '0' || c > '9') return false;
        return true;
    };
    if (x == std::string::npos) return {};
    const std::string_view a = std::string_view(value).substr(0, x);
    const std::string_view b = std::string_view(value).substr(x + 1);
    if (!digits(a) || !digits(b)) return {};
    return {str::parse_double(a), str::parse_double(b)};
}

MergedForumTags merge_forum_tags(const std::vector<ForumTag>& existing, const std::vector<std::string>& want) {
    constexpr std::size_t MAX = 20;
    static const std::set<std::string> retired{"壁打ち", "壁打", "sparring", "学習", "learning"};
    MergedForumTags out;
    std::set<std::string> have;
    for (const auto& t : existing) {
        if (retired.count(str::to_lower(str::trim(t.name)))) continue;
        out.tags.push_back(t);
        have.insert(t.name);
    }
    for (const auto& name : want) {
        if (have.count(name)) continue;
        if (out.tags.size() >= MAX) break;
        ForumTag t;
        t.name = name;
        out.tags.push_back(t);
        have.insert(name);
        out.added.push_back(name);
    }
    return out;
}

std::vector<std::string> ensure_forum_flow_tags(DiscordApi& api, const Channel& forum) {
    auto merged = merge_forum_tags(forum.available_tags);
    if (merged.added.empty() && merged.tags.size() == forum.available_tags.size()) return {};
    Json tags = Json::array();
    for (const auto& t : merged.tags) tags.push_back(to_json(t));
    api.modify_channel(forum.id, Json{{"available_tags", tags}});
    return merged.added;
}

std::optional<Channel> ensure_discussion_forum(DiscordApi& api, const std::string& guild_id, const std::string& forum_name) {
    try {
        api.get_guild(guild_id);
    } catch (const std::exception&) {
        return std::nullopt;
    }

    // 既存の同名フォーラムを探す。
    try {
        for (const auto& c : api.get_guild_channels(guild_id)) {
            if (c.type != channel_type::GuildForum || c.name.value_or("") != forum_name) continue;
            std::vector<std::string> added;
            try {
                added = ensure_forum_flow_tags(api, c);
            } catch (const std::exception&) {
            }
            if (!added.empty()) log::info("  forum-tags: #" + forum_name + " にタグ補充 [" + str::join(added, ", ") + "]");
            return c;
        }
    } catch (const std::exception&) {
        /* fetch 失敗 → 作成を試みる */
    }

    // 無ければ全タグ付きで作成する。
    try {
        auto merged = merge_forum_tags({});
        Json tags = Json::array();
        for (const auto& t : merged.tags) tags.push_back(to_json(t));
        Json body{{"name", forum_name},
                  {"type", channel_type::GuildForum},
                  {"available_tags", tags},
                  {"topic", "議論したいテーマを投稿してください。議論タイプ (企画/議論・改善/議論・壁打ち) のタグを 1 つ付けます。"}};
        auto created = api.create_guild_channel(guild_id, body, std::string("Discutere 議論フォーラム (新フロー入口)"));
        log::info("  forum-tags: 議論フォーラム #" + forum_name + " を作成 (タグ " + std::to_string(merged.tags.size()) + " 件)");
        return created;
    } catch (const std::exception& e) {
        log::warn("  forum-tags: 議論フォーラム作成失敗 (" + forum_name + "): " + e.what());
        return std::nullopt;
    }
}

} // namespace di::discord_hook
