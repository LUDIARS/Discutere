#include "flow/paper_review_intent.hpp"

#include <algorithm>
#include <array>
#include <set>

#include "base/strings.hpp"
#include "flow/text_scan.hpp"

namespace di::flow {

namespace {

struct Range {
    std::uint32_t first;
    std::uint32_t last;
};

constexpr Range kLetterNumber[] = {
#include "flow/unicode_letter_number.inc"
};

// VOICES_RE の展開: (外部の声|ユーザー?の声|プレイヤーの声|生の声|口コミ|(感想|レビュー|コメント|反応)を?(取り込|取りこ|集め|拾|追加|入れ))
const std::vector<std::string>& voices_phrases() {
    static const std::vector<std::string> list = [] {
        std::vector<std::string> out = {"外部の声", "ユーザーの声", "ユーザの声", "プレイヤーの声", "生の声", "口コミ"};
        for (const char* a : {"感想", "レビュー", "コメント", "反応"})
            for (const char* o : {"を", ""})
                for (const char* v : {"取り込", "取りこ", "集め", "拾", "追加", "入れ"})
                    out.push_back(std::string(a) + o + v);
        return out;
    }();
    return list;
}

// SIMILAR_GAMES_RE の展開
const std::vector<std::string>& similar_phrases() {
    static const std::vector<std::string> list = [] {
        std::vector<std::string> out;
        for (const char* p : {"類似する", "類似の", "類似", "似たような", "似た", "似ている"})
            for (const char* n : {"ゲーム", "作品", "タイトル"}) out.push_back(std::string(p) + n);
        for (const char* p : {"参考になる", "参考の", "参考"})
            for (const char* n : {"ゲーム", "作品"}) out.push_back(std::string(p) + n);
        out.emplace_back("メカニクスの代替");
        for (const char* p : {"代替の", "代替"})
            for (const char* n : {"ゲーム", "メカニクス"}) out.push_back(std::string(p) + n);
        return out;
    }();
    return list;
}

// FILLER_RE の選択肢を JS の試行順 (貪欲な省略可能部分は「付きが先」) に並べたもの。
// 各位置で最初に一致した語を空白に置き換える (/g の replace と同じ)。
const std::vector<std::string>& filler_phrases() {
    static const std::vector<std::string> list = {
        "類似する", "類似の", "類似", "似たような", "似た", "似ている", "参考になる", "参考の", "参考", "代替の", "代替",
        "メカニクス", "ゲーム", "作品", "タイトル", "外部の声", "ユーザーの声", "ユーザの声", "プレイヤーの声", "生の声",
        "口コミ", "感想", "レビュー", "コメント", "反応", "として", "を", "に", "で", "から", "の", "は", "も", "追加",
        "取り込んで", "取り込む", "取り込み", "取り込", "取りこんで", "取りこむ", "取りこ", "集めて", "集める", "集め",
        "拾って", "拾う", "拾", "入れて", "入れる", "入れ", "参照", "使って", "使う", "使", "して", "ください", "下さい",
        "お願いします", "お願い", "反映",
    };
    return list;
}

bool contains_any(const std::string& text, const std::vector<std::string>& phrases) {
    for (const auto& p : phrases)
        if (text.find(p) != std::string::npos) return true;
    return false;
}

// [...text.matchAll(/[「『"“]([^」』"”]+)[」』"”]/g)].map(m => m[1].trim()).filter(Boolean)
std::vector<std::string> quoted_terms(const std::string& text) {
    std::vector<std::string> out;
    for (const auto& seg : text_scan::quoted_segments(text, 1)) {
        std::string t = str::trim(seg);
        if (!t.empty()) out.push_back(std::move(t));
    }
    return out;
}

bool is_term_separator(std::uint32_t cp) {
    return cp == U'、' || cp == ',' || cp == U'，' || cp == U'・' || cp == '/' || cp == U'／' || str::is_js_space(cp);
}

bool is_trailing_punct(std::uint32_t cp) {
    return cp == U'。' || cp == '.' || cp == '!' || cp == U'！' || cp == '?' || cp == U'？';
}

// s.replace(/[...]+$/u, "") — 末尾から当てはまる文字を落とす
template <class Pred>
std::string strip_trailing(const std::string& s, Pred pred) {
    const auto cps = str::codepoints(s);
    std::size_t end = cps.size();
    while (end > 0 && pred(cps[end - 1])) --end;
    if (end == cps.size()) return s;
    return str::from_codepoints(std::vector<std::uint32_t>(cps.begin(), cps.begin() + static_cast<std::ptrdiff_t>(end)));
}

std::string replace_fillers(const std::string& body) {
    const auto& phrases = filler_phrases();
    std::string out;
    std::size_t i = 0;
    while (i < body.size()) {
        bool matched = false;
        for (const auto& p : phrases) {
            if (body.compare(i, p.size(), p) == 0) {
                out.push_back(' ');
                i += p.size();
                matched = true;
                break;
            }
        }
        if (matched) continue;
        std::size_t j = i;
        str::next_codepoint(body, j);
        out.append(body, i, j - i);
        i = j;
    }
    return out;
}

// 引用が無い時の素朴な抽出: 「:」以降 (無ければ全体) から言い回しを落として区切る。
std::vector<std::string> loose_terms(const std::string& text) {
    std::size_t colon = std::string::npos;
    {
        const auto a = text.find(':');
        const auto b = text.find("：");
        colon = std::min(a, b);
    }
    const std::string body = colon != std::string::npos ? text.substr(colon + (text.compare(colon, 1, ":") == 0 ? 1 : 3)) : text;
    const std::string replaced = replace_fillers(body);
    // .split(/[、,，・/／\s]+|と(?=\S)/)
    std::vector<std::string> pieces;
    std::string current;
    std::size_t i = 0;
    while (i < replaced.size()) {
        std::size_t j = i;
        const auto cp = str::next_codepoint(replaced, j);
        if (is_term_separator(cp)) {
            pieces.push_back(std::move(current));
            current.clear();
            i = j;
            while (i < replaced.size()) {
                std::size_t k = i;
                if (!is_term_separator(str::next_codepoint(replaced, k))) break;
                i = k;
            }
            continue;
        }
        if (cp == U'と' && j < replaced.size()) {
            std::size_t k = j;
            if (!str::is_js_space(str::next_codepoint(replaced, k))) {
                pieces.push_back(std::move(current));
                current.clear();
                i = j;
                continue;
            }
        }
        current.append(replaced, i, j - i);
        i = j;
    }
    pieces.push_back(std::move(current));
    std::vector<std::string> out;
    for (const auto& piece : pieces) {
        const std::string t = str::trim(strip_trailing(piece, is_trailing_punct));
        if (str::js_length(t) >= 2) out.push_back(t);
    }
    return out;
}

std::vector<std::string> uniq(const std::vector<std::string>& values) {
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (const auto& v : values)
        if (seen.insert(v).second) out.push_back(v);
    return out;
}

// 調整指示になっていない短い返信 (相づち・否定・別の質問への回答)。ペーパーを書き換えない。
const std::set<std::string>& non_instruction_replies() {
    static const std::set<std::string> set = {
        "ない", "無い", "なし", "無し", "ないです", "なしです", "ありません", "特にない", "特になし",
        "とくにない", "とくになし", "なにもない", "何もない", "大丈夫", "だいじょうぶ", "大丈夫です",
        "はい", "いいえ", "うん", "ううん", "いいよ", "りょうかい", "了解", "わかった", "分かった",
        "ありがとう", "ありがとうございます", "どうも", "そう", "そうです", "違う", "ちがう",
        "no", "none", "nope", "yes", "yep", "thanks", "thx",
    };
    return set;
}

} // namespace

bool is_letter_or_number(std::uint32_t cp) {
    const auto* end = std::end(kLetterNumber);
    const auto* it = std::upper_bound(std::begin(kLetterNumber), end, cp,
                                      [](std::uint32_t value, const Range& r) { return value < r.first; });
    if (it == std::begin(kLetterNumber)) return false;
    --it;
    return cp >= it->first && cp <= it->last;
}

std::string paper_review_intent_label(const PaperReviewIntentKind& kind) {
    if (kind == "adjust") return "議論内容の調整";
    if (kind == "voices") return "外部の声の取り込み";
    if (kind == "similar_games") return "類似するゲーム (メカニクスの代替)";
    return "";
}

bool is_non_instruction_reply(std::string_view raw) {
    std::string t = str::to_lower(str::trim(raw));
    // .replace(/[。、.,!！?？~〜ー\s]+$/u, "")
    t = strip_trailing(t, [](std::uint32_t cp) {
        return cp == U'。' || cp == U'、' || cp == '.' || cp == ',' || cp == '!' || cp == U'！' || cp == '?' ||
               cp == U'？' || cp == '~' || cp == U'〜' || cp == U'ー' || str::is_js_space(cp);
    });
    // .replace(/^[\s「『]+|[」』]+$/gu, "") — 先頭の連続と末尾の連続を落とす (2 つの文字集合は重ならない)
    {
        const auto cps = str::codepoints(t);
        std::size_t begin = 0;
        while (begin < cps.size() && (str::is_js_space(cps[begin]) || cps[begin] == U'「' || cps[begin] == U'『')) ++begin;
        std::size_t end = cps.size();
        while (end > begin && (cps[end - 1] == U'」' || cps[end - 1] == U'』')) --end;
        t = str::from_codepoints(std::vector<std::uint32_t>(cps.begin() + static_cast<std::ptrdiff_t>(begin),
                                                            cps.begin() + static_cast<std::ptrdiff_t>(end)));
    }
    if (str::js_length(t) <= 1) return true;
    bool has_letter = false;
    for (auto cp : str::codepoints(t)) {
        if (is_letter_or_number(cp)) {
            has_letter = true;
            break;
        }
    }
    if (!has_letter) return true;  // 記号・絵文字だけ
    return non_instruction_replies().count(t) > 0;
}

PaperReviewIntent classify_paper_review_intent(std::string_view raw) {
    const std::string text = str::trim(raw);
    if (contains_any(text, voices_phrases())) return {"voices", text, uniq(quoted_terms(text))};
    if (contains_any(text, similar_phrases())) {
        const auto quoted = quoted_terms(text);
        return {"similar_games", text, uniq(quoted.empty() ? loose_terms(text) : quoted)};
    }
    return {"adjust", text, {}};
}

Json to_json(const PaperReviewIntent& intent) {
    Json o = Json::object();
    o["kind"] = intent.kind;
    o["text"] = intent.text;
    o["terms"] = intent.terms;
    return o;
}

} // namespace di::flow
