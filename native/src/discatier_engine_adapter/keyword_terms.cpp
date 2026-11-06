#include "base/sort_by_index.hpp"
#include "discatier_engine_adapter/keyword_terms.hpp"

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <unordered_set>

#include "base/strings.hpp"

namespace di::discatier_engine_adapter {
namespace {

/** キーワードとして無意味な定型・汎用語。 */
bool is_stop(const std::string& t) {
    static const std::unordered_set<std::string> stop = {"について", "とは", "なぜ", "どう", "どこ", "面白い", "面白いか", "面白さ",
                                                         "本当に",   "ゲーム", "こと", "もの", "それ", "これ", "ため"};
    return stop.count(t) > 0;
}

/** [\s、。，．・,.;:：；「」『』（）()【】\[\]"'!?！？〜~…\/|] の 1 文字。 */
bool is_split_char(std::uint32_t c) {
    if (str::is_js_space(c)) return true;
    static const std::u32string chars = U"、。，．・,.;:：；「」『』（）()【】[]\"'!?！？〜~…/|";
    return chars.find(static_cast<char32_t>(c)) != std::u32string::npos;
}

/** 複数文字の区切り (正規表現の選択肢の順)。 */
const std::vector<std::u32string>& split_words() {
    static const std::vector<std::u32string> words = {U"について", U"という", U"からは", U"までに", U"だろう",
                                                      U"でしょう", U"ですか", U"なのか", U"から",   U"まで",
                                                      U"より",     U"など",   U"って",   U"とか",   U"けど"};
    return words;
}

/** 単一文字のひらがな助詞 [のはがをにへとやもでか]。 */
bool is_particle(std::uint32_t c) {
    static const std::u32string chars = U"のはがをにへとやもでか";
    return chars.find(static_cast<char32_t>(c)) != std::u32string::npos;
}

/** text.split(SPLIT) */
std::vector<std::string> split_pieces(const std::string& text) {
    const auto cps = str::codepoints(text);
    std::vector<std::string> pieces;
    std::vector<std::uint32_t> current;
    std::size_t i = 0;
    while (i < cps.size()) {
        std::size_t consumed = 0;
        if (is_split_char(cps[i])) {
            consumed = 1;
            while (i + consumed < cps.size() && is_split_char(cps[i + consumed])) ++consumed;
        } else {
            for (const auto& w : split_words()) {
                if (i + w.size() > cps.size()) continue;
                bool match = true;
                for (std::size_t k = 0; k < w.size() && match; ++k) match = cps[i + k] == static_cast<std::uint32_t>(w[k]);
                if (match) {
                    consumed = w.size();
                    break;
                }
            }
            if (consumed == 0 && is_particle(cps[i])) consumed = 1;
        }
        if (consumed > 0) {
            pieces.push_back(str::from_codepoints(current));
            current.clear();
            i += consumed;
        } else {
            current.push_back(cps[i]);
            ++i;
        }
    }
    pieces.push_back(str::from_codepoints(current));
    return pieces;
}

} // namespace

std::vector<std::string> extract_key_terms(const std::vector<std::string>& terms, std::size_t max) {
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    for (const auto& raw : terms) {
        for (const auto& piece : split_pieces(str::to_lower(raw))) {
            std::string t = str::trim(piece);
            if (str::js_length(t) >= 2 && !is_stop(t) && seen.insert(t).second) out.push_back(std::move(t));
        }
    }
    // 長い語ほど specific (ゲーム名等) なので優先して残す (JS の sort は安定)。
    di::stable_sort_by_index(out,
                     [](const std::string& a, const std::string& b) { return str::js_length(a) > str::js_length(b); });
    if (out.size() > max) out.resize(max);
    return out;
}

} // namespace di::discatier_engine_adapter
