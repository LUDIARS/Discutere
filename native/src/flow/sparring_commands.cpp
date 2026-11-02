#include "flow/sparring_commands.hpp"

#include "base/strings.hpp"

namespace di::flow {

namespace {

// 句読点・空白・装飾を除いた正規化: text.trim().replace(/[。、．，.!?！？\s「」『』]/g, "")
std::string normalize(std::string_view text) {
    const std::string t = str::trim(text);
    std::string out;
    std::size_t i = 0;
    while (i < t.size()) {
        const std::size_t start = i;
        const std::uint32_t cp = str::next_codepoint(t, i);
        const bool drop = cp == U'。' || cp == U'、' || cp == U'．' || cp == U'，' || cp == '.' || cp == '!' ||
                          cp == '?' || cp == U'！' || cp == U'？' || cp == U'「' || cp == U'」' || cp == U'『' ||
                          cp == U'』' || str::is_js_space(cp);
        if (!drop) out.append(t, start, i - start);
    }
    return out;
}

const char* const kEndTokens[] = {"おわり", "終わり", "おわる", "終わる", "終了", "終わりにして", "おしまい"};
const char* const kSummarizeTokens[] = {"まとめて", "まとめ"};
// /終わ(らな|れな|りた[くか])|おわ(らな|れな)/
const char* const kNegations[] = {"終わらな", "終われな", "終わりたく", "終わりたか", "おわらな", "おわれな"};

} // namespace

SparringCommand detect_sparring_command(std::string_view text) {
    const std::string n = normalize(text);
    if (n.empty()) return "none";
    const bool is_short = str::js_length(n) <= 16;

    // 終了が優先 (「まとめておわり」のような場合も終了)
    bool any_end = false;
    for (const char* t : kEndTokens) any_end = any_end || str::contains(n, t);
    if (any_end) {
        // 「終わらない」「終われない」等の否定は除外
        for (const char* neg : kNegations)
            if (str::contains(n, neg)) return "none";
        bool edge = false;
        for (const char* t : kEndTokens) edge = edge || str::starts_with(n, t) || str::ends_with(n, t);
        if (is_short || edge) return "end";
    }
    bool any_summarize = false;
    for (const char* t : kSummarizeTokens) any_summarize = any_summarize || str::contains(n, t);
    if (any_summarize && (is_short || str::starts_with(n, "まとめ"))) return "summarize";
    return "none";
}

} // namespace di::flow
