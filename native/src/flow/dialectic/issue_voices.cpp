#include "flow/dialectic/issue_voices.hpp"

#include "base/strings.hpp"

namespace di::flow::dialectic {

namespace {
/** 1 件の声の最大文字数 (プロンプトの肥大を防ぐ)。 */
constexpr std::size_t VOICE_CHARS = 160;
} // namespace

std::string render_issue_voices(const std::vector<ContextVoice>& voices) {
    if (voices.empty()) return "";
    std::vector<std::string> lines{"# プレイヤーの声 (集めた外部の声。根拠のデータとして使う)",
                                   "主張・根拠・反論のデータには、まずここにある実際の声を [V番号] で引いて使う。声に無いことを事実として作らない。"};
    for (std::size_t i = 0; i < voices.size(); ++i) {
        const std::string body = str::squish(voices[i].content);
        const std::string clipped = str::js_length(body) > VOICE_CHARS ? str::js_slice(body, 0, VOICE_CHARS) + "…" : body;
        lines.push_back("- [V" + std::to_string(i + 1) + "] " + clipped + "（出所: " + voices[i].source + "）");
    }
    return str::join(lines, "\n");
}

} // namespace di::flow::dialectic
