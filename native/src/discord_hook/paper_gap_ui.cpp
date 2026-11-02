#include "discord_hook/paper_gap_ui.hpp"

#include "base/strings.hpp"

namespace di::discord_hook {

const std::vector<PaperGapTypeDef>& paper_gap_types() {
    static const std::vector<PaperGapTypeDef> v{
        {"game_content", "ゲーム内容", "ゲーム内容・ルール・体験前提の不足"},
        {"user_reaction", "ユーザの反応", "レビュー・コメント・想定反応の不足"},
        {"spec", "仕様書", "仕様書や参考資料からの不足"},
        {"mechanics", "システム/メカニクス", "システムやメカニクス説明の不足"},
        {"project_code", "Anatomiaコード", "Anatomiaプロジェクトコードからの不足"},
        {"other", "その他", "分類しづらい不足事項"},
    };
    return v;
}

Json paper_gap_type_choices() {
    Json out = Json::array();
    for (const auto& t : paper_gap_types()) out.push_back(Json{{"name", t.name}, {"value", t.value}});
    return out;
}

bool is_paper_gap_type(const std::string& value) {
    for (const auto& t : paper_gap_types())
        if (value == t.value) return true;
    return false;
}

std::string label_for_paper_gap_type(const std::string& type) {
    for (const auto& t : paper_gap_types())
        if (type == t.value) return t.name;
    return type;
}

namespace {
Json row(Json input) { return Json{{"type", 1}, {"components", Json::array({std::move(input)})}}; }
Json text_input(const char* id, const char* label, const char* placeholder, bool required, int style) {
    return Json{{"type", 4}, {"custom_id", id}, {"label", label}, {"placeholder", placeholder}, {"required", required}, {"style", style}};
}
} // namespace

Json build_paper_gap_modal(const std::string& type, const std::string& channel_id) {
    const std::string label = label_for_paper_gap_type(type);
    return Json{{"custom_id", std::string(PAPER_GAP_MODAL_PREFIX) + ":" + type + ":" + channel_id},
                {"title", "不足情報: " + label},
                {"components", Json::array({
                                   row(text_input(paper_gap_field_ids::target, "対象・見出し", "例: バトルテンポ / 成長曲線 / 想定ユーザ", false, 1)),
                                   row(text_input(paper_gap_field_ids::detail, "追加・修正したい内容",
                                                  "ペーパーに反映したい不足情報を具体的に入力してください", true, 2)),
                                   row(text_input(paper_gap_field_ids::source, "根拠・情報ソース", "URL、ファイル名、発言元など。なければ空欄", false, 1)),
                               })}};
}

std::optional<PaperGapModalId> parse_paper_gap_modal_custom_id(const std::string& custom_id) {
    const std::string prefix = std::string(PAPER_GAP_MODAL_PREFIX) + ":";
    if (!str::starts_with(custom_id, prefix)) return std::nullopt;
    // rest.split(":") の先頭 2 要素 ([rawType, channelId])。
    const auto parts = str::split(custom_id.substr(prefix.size()), ":");
    if (parts.size() < 2 || parts[0].empty() || parts[1].empty() || !is_paper_gap_type(parts[0])) return std::nullopt;
    return PaperGapModalId{parts[0], parts[1]};
}

std::string format_paper_gap_review_instruction(const PaperGapInstructionInput& input) {
    std::vector<std::string> lines{"ディスカッションペーパーの不足事項として「" + label_for_paper_gap_type(input.type) + "」を追加・反映してください。"};
    if (input.target && !str::trim(*input.target).empty()) lines.push_back("対象: " + str::trim(*input.target));
    lines.push_back("内容: " + str::trim(input.detail));
    if (input.source && !str::trim(*input.source).empty()) lines.push_back("根拠・情報ソース: " + str::trim(*input.source));
    return str::join(lines, "\n");
}

} // namespace di::discord_hook
