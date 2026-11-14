#include "flow/transcript/html.hpp"

#include <algorithm>
#include <map>
#include <vector>

#include "api/web_page.hpp"
#include "api/web_template.hpp"
#include "base/strings.hpp"
#include "base/time.hpp"
#include "flow/transcript/paper_md_html.hpp"

namespace di::flow::transcript {

namespace {

std::string json_string(const Json& obj, const char* key) {
    const auto it = obj.find(key);
    return it != obj.end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::int64_t json_int(const Json& obj, const char* key) {
    const auto it = obj.find(key);
    return it != obj.end() && it->is_number() ? it->get<std::int64_t>() : 0;
}

bool json_bool(const Json& obj, const char* key) {
    const auto it = obj.find(key);
    return it != obj.end() && it->is_boolean() && it->get<bool>();
}

/** page.html の utteranceMarks と同じ並び。 */
std::string utterance_marks(const Json& u, const std::string& stance, const std::string& role) {
    std::string out = "<span class=\"badge\">" + str::escape_html(utterance_stance_label(stance, role)) + "</span>";
    const auto votes = json_int(u, "votes");
    if (votes > 0) out += "<span class=\"mark vote\">投票 " + std::to_string(votes) + "</span>";
    if (json_bool(u, "isWinner")) out += "<span class=\"mark win\">採択</span>";
    const auto auf = u.find("roundAufhebung");
    if (auf != u.end() && auf->is_array() && !auf->empty())
        out += "<span class=\"mark auf\">止揚 " + std::to_string(auf->size()) + "</span>";
    if (json_bool(u, "isError")) out += "<span class=\"mark\">エラー</span>";
    return out;
}

std::string utterance_card(const Json& u) {
    const std::string stance = json_string(u, "stance");
    const std::string role = json_string(u, "role");
    std::string who = json_string(u, "displayName");
    if (who.empty()) who = json_string(u, "personaName");
    std::string cls = "u " + utterance_stance_class(stance, role);
    if (json_bool(u, "isError")) cls += " err";
    return "          <div class=\"" + cls + "\"><div class=\"who\"><span>" + str::escape_html(who) + "</span><span class=\"marks\">" +
           utterance_marks(u, stance, role) + "</span></div><div class=\"text\">" + str::escape_html(json_string(u, "text")) +
           "</div></div>\n";
}

std::string utterance_cards(const Json& utterances, std::size_t max_bytes) {
    std::string out;
    std::size_t shown = 0;
    for (const auto& u : utterances) {
        std::string card = utterance_card(u);
        if (out.size() + card.size() > max_bytes) break;
        out += card;
        ++shown;
    }
    if (shown == 0 && utterances.empty()) out = "          <p class=\"muted\">発言はありません。</p>\n";
    if (shown < utterances.size())
        out += "          <p class=\"muted\">以降の " + std::to_string(utterances.size() - shown) + " 件は容量の上限を超えるため省きました。</p>\n";
    return out;
}

std::string paper_section(const std::optional<std::string>& md) {
    if (!md) return "";
    return "    <div id=\"paperPanel\">\n      <div class=\"panel-head\"><h2>ディスカッションペーパー</h2></div>\n"
           "      <div id=\"paperBody\" class=\"paper-md\">" +
           render_paper_md_html(*md) + "</div>\n    </div>";
}

std::string conclusion_section(const Json& conclusion) {
    if (!conclusion.is_string() || conclusion.get<std::string>().empty()) return "";
    return "      <div id=\"conclusion\">\n        <div class=\"panel-head\"><h2>収束結果</h2></div>\n"
           "        <div id=\"conclusionBody\">" +
           str::escape_html(conclusion.get<std::string>()) + "</div>\n      </div>";
}

/** 「発言 N 件 / ラウンド M / 出力 YYYY-MM-DD HH:MM (UTC)」。 */
std::string meta_line(const TranscriptHtmlInput& input) {
    std::int64_t max_round = 0;
    for (const auto& u : input.snapshot.utterances) max_round = std::max(max_round, json_int(u, "round"));
    std::vector<std::string> parts;
    parts.push_back("発言 " + std::to_string(input.snapshot.utterances.size()) + " 件");
    if (max_round > 0) parts.push_back("ラウンド " + std::to_string(max_round));
    parts.push_back(input.snapshot.concluded ? "結論あり" : "結論なし");
    if (input.generatedAtMs) {
        const std::string iso = time::iso_utc(*input.generatedAtMs);  // YYYY-MM-DDTHH:MM:SS.sssZ
        parts.push_back("出力 " + iso.substr(0, 10) + " " + iso.substr(11, 5) + " (UTC)");
    }
    return str::escape_html(str::join(parts, " / "));
}

} // namespace

std::string utterance_stance_class(const std::string& stance, const std::string& role) {
    if (role == "user") return "user";
    if (stance == "pro" || stance == "con" || stance == "opinion") return stance;
    return "neutral";
}

std::string utterance_stance_label(const std::string& stance, const std::string& role) {
    if (role == "user") return "人間";
    if (stance == "pro") return "賛成派";
    if (stance == "con") return "反対派";
    if (stance.empty() || stance == "opinion" || stance == "neutral") return "中立";
    return stance;
}

std::string render_transcript_html(const TranscriptHtmlInput& input, std::size_t cards_max_bytes) {
    const std::string theme = input.theme.empty() ? std::string("(議題なし)") : input.theme;
    std::map<std::string, std::string> values;
    values["title"] = str::escape_html(theme);
    values["flowLabel"] = str::escape_html(input.flowLabel);
    values["meta"] = meta_line(input);
    values["liveClass"] = input.snapshot.paperMd ? "" : "no-paper";
    values["paperSection"] = paper_section(input.snapshot.paperMd);
    values["utterances"] = utterance_cards(input.snapshot.utterances, cards_max_bytes);
    values["conclusionSection"] = conclusion_section(input.snapshot.conclusion);
    return api::fill_template(api::web_page("flow/transcript.html"), values);
}

} // namespace di::flow::transcript
