#include "flow/transcript/digest.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "base/strings.hpp"
#include "flow/transcript/html.hpp"

namespace di::flow::transcript {

namespace {

std::string json_string(const Json& obj, const char* key) {
    const auto it = obj.find(key);
    return it != obj.end() && it->is_string() ? it->get<std::string>() : std::string();
}

/** 改行を空白に詰め、UTF-16 で max 単位を超えたら末尾を「…」にする。 */
std::string excerpt(const std::string& text, std::size_t max) {
    const std::string flat = str::squish(text);
    if (str::js_length(flat) <= max) return flat;
    return str::js_slice(flat, 0, static_cast<std::int64_t>(max > 0 ? max - 1 : 0)) + "…";
}

std::string stance_counts(const Json& utterances) {
    // 表示順を固定する (賛成派 → 反対派 → 中立 → 人間 → その他)。
    std::vector<std::pair<std::string, std::size_t>> counts = {{"賛成派", 0}, {"反対派", 0}, {"中立", 0}, {"人間", 0}};
    for (const auto& u : utterances) {
        const std::string label = utterance_stance_label(json_string(u, "stance"), json_string(u, "role"));
        auto it = std::find_if(counts.begin(), counts.end(), [&](const auto& c) { return c.first == label; });
        if (it == counts.end()) counts.emplace_back(label, 1);
        else ++it->second;
    }
    std::vector<std::string> parts;
    for (const auto& [label, n] : counts)
        if (n > 0) parts.push_back(label + " " + std::to_string(n));
    return str::join(parts, " / ");
}

std::string winners_block(const Json& utterances) {
    std::vector<std::string> lines;
    std::size_t total = 0;
    for (const auto& u : utterances) {
        const auto w = u.find("isWinner");
        if (w == u.end() || !w->is_boolean() || !w->get<bool>()) continue;
        ++total;
        if (lines.size() >= kDigestMaxWinners) continue;
        std::string who = json_string(u, "displayName");
        if (who.empty()) who = json_string(u, "personaName");
        lines.push_back("- " + excerpt(who, 40) + ": " + excerpt(json_string(u, "text"), 140));
    }
    if (lines.empty()) return "";
    if (total > lines.size()) lines.push_back("- ほか " + std::to_string(total - lines.size()) + " 件");
    return "**採択された意見**\n" + str::join(lines, "\n") + "\n";
}

std::int64_t max_round(const Json& utterances) {
    std::int64_t r = 0;
    for (const auto& u : utterances) {
        const auto it = u.find("round");
        if (it != u.end() && it->is_number_integer()) r = std::max(r, it->get<std::int64_t>());
    }
    return r;
}

} // namespace

std::string render_transcript_digest(const DigestInput& input) {
    const auto& utt = input.snapshot.utterances;
    std::string head = "📄 **議論のまとめ** — " + excerpt(input.theme.empty() ? std::string("(議題なし)") : input.theme, 200) + "\n";
    std::vector<std::string> scale;
    if (!input.flowLabel.empty()) scale.push_back(input.flowLabel);
    if (const auto r = max_round(utt); r > 0) scale.push_back("ラウンド " + std::to_string(r));
    std::string counts = stance_counts(utt);
    scale.push_back("発言 " + std::to_string(utt.size()) + " 件" + (counts.empty() ? std::string() : " (" + counts + ")"));
    head += str::join(scale, " / ") + "\n";
    const std::string winners = winners_block(utt);
    const std::string tail = "チャット形式の全文は添付の HTML をブラウザで開いてください。";

    const std::string conclusion = input.snapshot.conclusion.is_string() ? input.snapshot.conclusion.get<std::string>() : std::string();
    std::string body = head + winners;
    const std::string conclusion_head = "**結論**\n";
    const std::size_t used = str::js_length(body) + str::js_length(conclusion_head) + str::js_length(tail) + 2;
    if (used < kDigestMaxLength) {
        const std::size_t room = kDigestMaxLength - used;
        const std::string c = conclusion.empty() ? std::string("(明確な結論には至りませんでした)") : conclusion;
        const std::string trimmed = str::js_length(c) <= room ? c : str::js_slice(c, 0, static_cast<std::int64_t>(room > 0 ? room - 1 : 0)) + "…";
        body += conclusion_head + trimmed + "\n";
    }
    body += tail;
    if (str::js_length(body) > kDigestMaxLength) body = str::js_slice(body, 0, static_cast<std::int64_t>(kDigestMaxLength - 1)) + "…";
    return body;
}

} // namespace di::flow::transcript
