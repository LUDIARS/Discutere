#include "discord_hook/command_defs.hpp"

#include "discord_hook/level_check.hpp"
#include "discord_hook/mechanics_check.hpp"
#include "discord_hook/paper_gap_ui.hpp"
#include "mechanics_check/contracts.hpp"

namespace di::discord_hook {

namespace {

Json option(const char* name, const char* description, int type, bool required) {
    Json o = Json::object();
    o["name"] = name;
    o["description"] = description;
    o["type"] = type;
    o["required"] = required;
    return o;
}

Json option_max(const char* name, const char* description, bool required, std::size_t max_length) {
    Json o = option(name, description, option_type::String, required);
    o["max_length"] = max_length;
    return o;
}

Json choice(const char* name, const char* value) { return Json{{"name", name}, {"value", value}}; }

Json command(const char* name, const char* description) {
    Json c = Json::object();
    c["name"] = name;
    c["description"] = description;
    return c;
}

Json command(const char* name, const char* description, Json options) {
    Json c = command(name, description);
    c["options"] = std::move(options);
    return c;
}

Json build() {
    const auto input_chars = mechanics_check::MECHANICS_CHECK_LIMITS.discordInputChars;
    Json defs = Json::array();
    defs.push_back(command(MECHANICS_CHECK_COMMAND_NAME, "企画のメカニクス整合性を単発診断 (logic_differences / design_gap)",
                           Json::array({
                               option_max("spec", "企画本文 (目指す体験とルール)", true, 6'000),
                               option_max("baseline", "比較する旧版や基準本文 (任意)", false, 6'000),
                               option_max("input", "references を含むJSON (模型は /level-check)", false, input_chars),
                           })));
    defs.push_back(command(LEVEL_CHECK_COMMAND_NAME, "配置・技能・予測可能性を単発診断 (logic_differences / design_gap)",
                           Json::array({
                               option_max("level", "場面本文 (配置・遭遇・適用ルール・目指す体験)", true, 6'000),
                               option_max("baseline", "比較する場面本文 (任意)", false, 6'000),
                               option_max("input", "references / playModel を含むJSON (任意)", false, input_chars),
                           })));
    defs.push_back(command("propose", "hypothesis (跳躍的仮説) を提案して自走議論を起こす",
                           Json::array({option("statement", "仮説を 1 文で", option_type::String, true)})));
    {
        Json mode = option("mode", "theory または emotion", option_type::String, true);
        mode["choices"] = Json::array({choice("theory", "theory"), choice("emotion", "emotion")});
        defs.push_back(command("validate", "hypothesis を検証する (theory=論理 / emotion=感情)", Json::array({mode})));
    }
    defs.push_back(command("integrate", "検証済 hypothesis を設計知見として統合 (採用)"));
    defs.push_back(command("reject", "hypothesis を棄却"));
    defs.push_back(command("discutere-status", "persona-engine の稼働状態を確認 (ephemeral)"));
    defs.push_back(command("discutere-queue", "現在の議論キュー (進行中 session / 未処理 gap / 仮説) を可視化"));
    defs.push_back(command("discutere-kill", "persona-engine の自走を停止/再開 (admin only)",
                           Json::array({option("enabled", "true で再開、false で停止", option_type::Boolean, true)})));
    defs.push_back(command("discutere-backup", "学習データを今すぐ S3 にバックアップ (admin only)"));
    defs.push_back(command("discutere-conclusions", "収束した議論の結論を一覧 / 指定で論述データ (議論ログ・止揚・高評価意見) を表示",
                           Json::array({option("gap", "結論の gap id を指定すると裏の論述データを表示 (省略で一覧)", option_type::String, false)})));
    {
        Json state = option("state", "絞り込み (省略で全件)", option_type::String, false);
        state["choices"] = Json::array({choice("draft (下書き)", "draft"), choice("live (進行中)", "live"),
                                        choice("concluded (結論あり)", "concluded")});
        defs.push_back(command("discutere-discussions", "議論一覧を表示 (下書き/進行中/結論あり。state で絞り込み)", Json::array({state})));
    }
    {
        Json type = option("type", "不足事項の種類", option_type::String, true);
        type["choices"] = paper_gap_type_choices();
        defs.push_back(command(PAPER_GAP_COMMAND_NAME, "Diのディスカッションペーパー不足事項を専用UIで投稿する", Json::array({type})));
    }
    defs.push_back(command("debate", "議題でパーティ議論を開始 (司会+キーマン+意見、想定発話数で続行/停止を問う)",
                           Json::array({option("topic", "議題 (例: ヴァンサバ風ゲームの面白さの核は?)", option_type::String, true)})));
    defs.push_back(command("persona-generate", "外部データ (クロール済の声) から憑依対象ペルソナを生成/更新する (admin only)",
                           Json::array({option("source", "source 種別で絞る (例: youtube / steam。省略で全体)", option_type::String, false)})));
    defs.push_back(command("economy-graph", "ゲームの内部経済導線と遊びの関係性をグラフ分析する",
                           Json::array({option("game", "ゲームタイトル (例: Hades, Hollow Knight, Balatro)", option_type::String, true)})));
    return defs;
}

} // namespace

const Json& DISCORD_COMMAND_DEFS() {
    static const Json defs = build();
    return defs;
}

} // namespace di::discord_hook
