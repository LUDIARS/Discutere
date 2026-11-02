#include "slack/slack_entry.hpp"

#include "base/strings.hpp"

namespace di::slack {

namespace {

SlackBlock plain_option(const std::string& value, const std::string& label) {
    return Json{{"text", Json{{"type", "plain_text"}, {"text", label}}}, {"value", value}};
}

std::vector<SlackBlock> select_blocks(const std::string& header, const std::string& action_id, const std::string& placeholder,
                                      const std::vector<std::pair<std::string, std::string>>& options) {
    Json opts = Json::array();
    for (const auto& [v, l] : options) opts.push_back(plain_option(v, l));
    return {
        Json{{"type", "section"}, {"text", Json{{"type", "mrkdwn"}, {"text", header}}}},
        Json{{"type", "actions"},
             {"elements", Json::array({Json{{"type", "static_select"},
                                            {"action_id", action_id},
                                            {"placeholder", Json{{"type", "plain_text"}, {"text", placeholder}}},
                                            {"options", opts}}})}},
    };
}

std::optional<std::string> str_of(const Json& obj, const char* key) {
    if (!obj.is_object()) return std::nullopt;
    auto it = obj.find(key);
    if (it != obj.end() && it->is_string()) return it->get<std::string>();
    return std::nullopt;
}

} // namespace

std::vector<SlackBlock> build_flow_pick_blocks() {
    return select_blocks("*議論タイプを選択してください* (必須)", SLACK_FLOW_PICK_ACTION, "議論タイプを選択",
                         {{"議論", "議論 (中立投票で世論をまとめる)"},
                          {"改善", "改善 (課題抽出 + 改善案)"},
                          {"学習", "学習 (外部の声を収集して KG に取り込む)"},
                          {"壁打ち", "壁打ち (あなたの発言に AI が応答し続ける)"}});
}

std::vector<SlackBlock> build_flow_settings_blocks() {
    return select_blocks("*進行量を選んで開始* (ラウンド数 × ターン数)", SLACK_FLOW_SETTINGS_ACTION, "進行量を選択",
                         {{"default", "標準で開始 (設定ファイルの既定)"},
                          {"2x4", "さくっと (2R×4T)"},
                          {"3x6", "標準量 (3R×6T)"},
                          {"5x8", "じっくり (5R×8T)"},
                          {"8x12", "とことん (8R×12T)"}});
}

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

bool is_thread_starter(const Json& event) {
    if (!event.is_object()) return false;
    if (str_of(event, "type") != std::optional<std::string>("message")) return false;
    if (auto subtype = str_of(event, "subtype"); subtype && !subtype->empty()) return false;
    if (event.contains("bot_id") && json_truthy(event["bot_id"])) return false;
    auto ts = str_of(event, "ts");
    if (!ts || ts->empty()) return false;
    auto thread_ts = str_of(event, "thread_ts");
    return !thread_ts || thread_ts->empty() || *thread_ts == *ts;
}

std::optional<std::string> first_selected_value(const Json& payload) {
    if (!payload.is_object() || !payload.contains("actions") || !payload["actions"].is_array() || payload["actions"].empty())
        return std::nullopt;
    const Json& first = payload["actions"][0];
    if (!first.is_object() || !first.contains("selected_option")) return std::nullopt;
    return str_of(first["selected_option"], "value");
}

std::optional<std::string> first_action_id(const Json& payload) {
    if (!payload.is_object() || !payload.contains("actions") || !payload["actions"].is_array() || payload["actions"].empty())
        return std::nullopt;
    return str_of(payload["actions"][0], "action_id");
}

} // namespace di::slack
