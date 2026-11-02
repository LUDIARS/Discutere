#include "persona_engine/facilitator/prompts.hpp"

#include <stdexcept>

#include "base/strings.hpp"

namespace di::persona_engine::facilitator {

namespace {
std::string transcript(const std::vector<RecentUtterance>& recent) {
    std::vector<std::string> lines;
    for (const auto& u : recent) lines.push_back("- " + u.speaker + ": " + u.content);
    std::string joined = str::join(lines, "\n");
    return joined.empty() ? "(まだ発言なし)" : joined;
}

std::string numbered(const std::vector<std::string>& items) {
    std::vector<std::string> lines;
    for (std::size_t i = 0; i < items.size(); ++i) lines.push_back(std::to_string(i + 1) + ". " + items[i]);
    return str::join(lines, "\n");
}
} // namespace

JsonLimits llm_json_limits() {
    JsonLimits l;
    l.max_bytes = 1u << 20;
    l.max_elements = 50'000;
    l.max_depth = 32;
    return l;
}

Json extract_json_object(std::string_view text) {
    const auto start = text.find('{');
    const auto end = text.rfind('}');
    if (start == std::string_view::npos || end == std::string_view::npos || end <= start)
        throw std::runtime_error("no JSON object in LLM output");
    return parse_json_or_throw(text.substr(start, end - start + 1), llm_json_limits());
}

PromptPair build_expand_prompt(const std::string& topic, const std::vector<std::string>& existing_persona_names,
                               const std::vector<RecentUtterance>& recent) {
    PromptPair p;
    p.system = std::string("あなたは議論の進行役 (ファシリテーター) です。 停滞した議論に、 まだ出ていない") +
               "新しい視点を持ち込む参加者を 1 人だけ作り、 その人の最初の一言を考えます。" +
               " 既存の参加者と立場が被らないようにしてください。 返答は JSON のみ。";
    std::string names = str::join(existing_persona_names, " / ");
    if (names.empty()) names = "(なし)";
    p.user = "# 議題\n" + topic + "\n\n" + "# 既に参加している立場\n" + names + "\n\n" + "# 直近の流れ\n" +
             transcript(recent) + "\n\n" +
             "# 指示\n上記と被らない新しい視点の参加者を 1 人作り、 議論を広げる開口一番を出してください。\n" +
             "返答は次の JSON のみ:\n" +
             "{\n"
             "  \"name\": \"<役割名 (短い、 例: コスト懐疑)>\",\n"
             "  \"display_name\": \"<日本人名 (例: 費田 渋)>\",\n"
             "  \"description\": \"<その人の立場・関心 (1 文)>\",\n"
             "  \"traits\": [\"<特徴>\", \"<特徴>\"],\n"
             "  \"speech_style\": \"<口調の特徴 (1 文)>\",\n"
             "  \"opening\": \"<開口一番。 新しい論点を 1〜2 文で。 鋭く具体的に>\"\n"
             "}";
    return p;
}

PromptPair build_aufhebung_judge_prompt(const std::string& topic, const std::vector<RecentUtterance>& recent,
                                        const std::vector<std::string>& stocked_summaries) {
    PromptPair p;
    p.system = std::string("あなたは議論の進行役です。 議論の中に『アウフヘーベン(止揚)』、 すなわち") +
               " 対立する複数の意見を否定し合うのではなく、 両者を活かして一段上の新しい結論へ統合した見解が" +
               " 新たに現れたかを判定します。 単なる賛成・繰り返し・部分的な指摘は止揚ではありません。 返答は JSON のみ。";
    const std::string stocked = stocked_summaries.empty() ? "(まだ無し)" : numbered(stocked_summaries);
    p.user = "# 議題\n" + topic + "\n\n" + "# 既にストック済みの止揚 (これと実質同じなら found=false)\n" + stocked + "\n\n" +
             "# 直近の議論\n" + transcript(recent) + "\n\n" +
             "# 指示\n直近の議論に、 既出と異なる新しい止揚が現れたか判定してください。 返答は次の JSON のみ:\n" +
             "{ \"found\": <true/false>, \"summary\": \"<止揚の要旨を 1〜2 文で。 found=false なら空文字>\" }";
    return p;
}

PromptPair build_converge_prompt(const std::string& topic, const std::vector<RecentUtterance>& recent,
                                 const std::optional<std::vector<std::string>>& aufhebungen,
                                 const std::optional<std::vector<std::string>>& top_opinions) {
    PromptPair p;
    p.system = std::string("あなたは議論の進行役 (ファシリテーター) です。 出揃った議論を中立の立場で要約し、") +
               " 到達した止揚 (統合された結論) と、 参加者の評価が高かった意見を軸に、 締めのまとめを作ります。 返答は JSON のみ。";
    const std::string auf = aufhebungen && !aufhebungen->empty() ? numbered(*aufhebungen) : "(特になし)";
    std::string top = "(特になし)";
    if (top_opinions && !top_opinions->empty()) {
        std::vector<std::string> lines;
        for (const auto& s : *top_opinions) lines.push_back("- " + s);
        top = str::join(lines, "\n");
    }
    p.user = "# 議題\n" + topic + "\n\n" + "# 到達した止揚 (最重要、 結論の軸にする)\n" + auf + "\n\n" +
             "# 評価の高かった意見 (リアクション順、 重視する。 「/ 人間」 付きは人間の意見なので最優先で尊重する)\n" + top +
             "\n\n" + "# これまでの議論\n" + transcript(recent) + "\n\n" +
             "# 指示\n上記の止揚と高評価意見を軸に、 議論の到達点を簡潔にまとめてください。 返答は次の JSON のみ:\n" +
             "{ \"summary\": \"<収束のまとめ (Discord 1 投稿に収まる長さ、 箇条書き可)>\" }";
    return p;
}

} // namespace di::persona_engine::facilitator
