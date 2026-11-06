#include "flow/web/routes_internal.hpp"

namespace di::flow::web {

namespace routes_detail {


bool is_letter_or_number(std::uint32_t cp) {
    const auto* end = std::end(kLetterNumber);
    const auto* it = std::upper_bound(std::begin(kLetterNumber), end, cp, [](std::uint32_t v, const Range& r) { return v < r.first; });
    if (it == std::begin(kLetterNumber)) return false;
    --it;
    return cp >= it->first && cp <= it->last;
}


bool ascii_ci_equal_at(std::string_view s, std::size_t pos, std::string_view lower_kw) {
    if (pos + lower_kw.size() > s.size()) return false;
    for (std::size_t i = 0; i < lower_kw.size(); ++i) {
        char c = s[pos + i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c != lower_kw[i]) return false;
    }
    return true;
}


std::size_t skip_js_spaces(std::string_view s, std::size_t p) {
    while (p < s.size()) {
        std::size_t next = p;
        if (!str::is_js_space(str::next_codepoint(s, next))) break;
        p = next;
    }
    return p;
}


std::string normalize_synthetic_voice_text(const std::string& raw) {
    std::string s = raw;
    // /^\s*\[synthetic\]\s*/i
    {
        const std::size_t p = skip_js_spaces(s, 0);
        if (ascii_ci_equal_at(s, p, "[synthetic]")) s = s.substr(skip_js_spaces(s, p + 11));
    }
    // /^\s*\[[^\]]*synthetic[^\]]*\]\s*/i
    {
        const std::size_t p = skip_js_spaces(s, 0);
        if (p < s.size() && s[p] == '[') {
            const std::size_t close = s.find(']', p + 1);
            if (close != std::string::npos) {
                const std::string inner = str::to_lower_ascii(std::string_view(s).substr(p + 1, close - p - 1));
                if (str::contains(inner, "synthetic")) s = s.substr(skip_js_spaces(s, close + 1));
            }
        }
    }
    return str::js_slice(str::trim(str::collapse_whitespace(s)), 0, 500);
}


/** key を UTF-16 のコード単位列にする (JS の文字列の length / slice と同じ数え方)。 */
std::u16string to_utf16(std::string_view s) {
    std::u16string out;
    for (std::size_t i = 0; i < s.size();) {
        std::uint32_t cp = str::next_codepoint(s, i);
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
    }
    return out;
}


std::string synthetic_similarity_key(const std::string& text) {
    const std::string lower = str::replace_all(str::to_lower(text), "[synthetic]", "");
    std::string out;
    for (std::size_t i = 0; i < lower.size();) {
        const std::size_t start = i;
        const auto cp = str::next_codepoint(lower, i);
        if (is_letter_or_number(cp)) out.append(lower, start, i - start);
    }
    return out;
}


std::set<std::uint64_t> synthetic_shingles(const std::u16string& key) {
    std::set<std::uint64_t> out;
    if (key.size() <= 2) {
        if (key.empty()) return out;
        const std::uint64_t a = key[0];
        const std::uint64_t b = key.size() > 1 ? key[1] : 0;
        out.insert((static_cast<std::uint64_t>(key.size()) << 32) | (a << 16) | b);
        return out;
    }
    for (std::size_t i = 0; i + 1 < key.size(); ++i)
        out.insert((2ull << 32) | (static_cast<std::uint64_t>(key[i]) << 16) | static_cast<std::uint64_t>(key[i + 1]));
    return out;
}


double synthetic_voice_similarity(const std::string& a, const std::string& b) {
    const std::string ak = synthetic_similarity_key(a);
    const std::string bk = synthetic_similarity_key(b);
    if (ak.empty() || bk.empty()) return 0;
    const auto a16 = to_utf16(ak);
    const auto b16 = to_utf16(bk);
    const bool a_short = a16.size() <= b16.size();
    const std::string& short_s = a_short ? ak : bk;
    const std::string& long_s = a_short ? bk : ak;
    if ((a_short ? a16.size() : b16.size()) >= 18 && str::contains(long_s, short_s)) return 1;
    const auto as = synthetic_shingles(a16);
    const auto bs = synthetic_shingles(b16);
    std::size_t overlap = 0;
    for (const auto s : as)
        if (bs.count(s)) ++overlap;
    const std::size_t uni = as.size() + bs.size() - overlap;
    return uni > 0 ? static_cast<double>(overlap) / static_cast<double>(uni) : 0;
}


std::string compact_synthetic_label(const Json* raw, const std::string& fallback) {
    if (!raw || !raw->is_string()) return fallback;
    const std::string text = str::js_slice(str::collapse_whitespace(str::trim(raw->get<std::string>())), 0, 32);
    return text.empty() ? fallback : text;
}


/** a ?? b ?? c (null / 無いキーを飛ばす)。 */
const Json* first_present(const Json* obj, std::initializer_list<const char*> keys) {
    if (!obj) return nullptr;
    for (const char* k : keys) {
        const auto it = obj->find(k);
        if (it != obj->end() && !it->is_null()) return &*it;
    }
    return nullptr;
}


std::string coerce_synthetic_polarity(const Json* raw) {
    if (!raw || !raw->is_string()) return "mixed";
    const std::string text = str::to_lower(str::trim(raw->get<std::string>()));
    if (text == "positive" || str::contains(text, "肯定") || str::contains(text, "賛成")) return "positive";
    if (text == "negative" || str::contains(text, "否定") || str::contains(text, "反対") || str::contains(text, "不満")) return "negative";
    return "mixed";
}


std::string polarity_label(const std::string& p) {
    if (p == "positive") return "肯定";
    if (p == "negative") return "否定";
    return "混合";
}


std::optional<SyntheticCandidate> coerce_synthetic_candidate(const Json& raw) {
    const Json* obj = raw.is_object() ? &raw : nullptr;
    std::string text;
    if (raw.is_string()) text = raw.get<std::string>();
    else if (obj && obj->contains("content") && (*obj)["content"].is_string()) text = (*obj)["content"].get<std::string>();
    const std::string content = normalize_synthetic_voice_text(text);
    if (content.empty()) return std::nullopt;
    SyntheticCandidate c;
    c.segment = compact_synthetic_label(first_present(obj, {"segment", "persona", "userSegment"}), "仮想ユーザ");
    c.concern = compact_synthetic_label(first_present(obj, {"concern", "issue", "topic"}), "未分類の論点");
    c.polarity = coerce_synthetic_polarity(first_present(obj, {"polarity", "sentiment"}));
    c.similarityText = c.segment + " " + c.concern + " " + content;
    c.content = "[synthetic] (" + c.segment + " / " + c.concern + " / " + polarity_label(c.polarity) + ") " + content;
    return c;
}


std::vector<SyntheticCandidate> select_diverse_synthetic(const Json& raw_items, std::size_t limit) {
    std::vector<SyntheticCandidate> candidates;
    if (raw_items.is_array()) {
        // LLM の応答由来なので候補数に上限を課す (TS は全件)。
        for (const auto& item : raw_items) {
            if (candidates.size() >= 200) break;
            if (auto c = coerce_synthetic_candidate(item)) candidates.push_back(std::move(*c));
        }
    }
    std::vector<SyntheticCandidate> selected;
    std::set<std::size_t> selected_indexes;
    std::set<std::string> used_slots;
    const std::vector<std::string> order = {"negative", "mixed", "positive", "negative", "mixed", "positive", "negative", "mixed"};
    auto try_pick = [&](const std::string* wanted) {
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            if (selected_indexes.count(i)) continue;
            const auto& c = candidates[i];
            if (wanted && c.polarity != *wanted) continue;
            const std::string slot = str::to_lower(c.segment) + "|" + str::to_lower(c.concern);
            if (used_slots.count(slot)) continue;
            bool similar = false;
            for (const auto& s : selected)
                if (synthetic_voice_similarity(s.similarityText, c.similarityText) >= 0.58) {
                    similar = true;
                    break;
                }
            if (similar) continue;
            selected.push_back(c);
            selected_indexes.insert(i);
            used_slots.insert(slot);
            return true;
        }
        return false;
    };
    for (const auto& p : order) {
        if (selected.size() >= limit) break;
        try_pick(&p);
    }
    while (selected.size() < limit && try_pick(nullptr)) {
        // 重ならない残りの候補で埋める。
    }
    return selected;
}


std::vector<LearningOpinion> synthesize_learning_opinions(const std::string& theme, const std::vector<GameMechanicEntry>& mechanics,
                                                          const std::string& mechanics_context, persona_engine::LLMClient& llm,
                                                          const std::function<void(const std::string&)>& warn) {
    std::string mechanics_text;
    if (!mechanics.empty()) {
        std::vector<std::string> lines;
        for (std::size_t i = 0; i < mechanics.size() && i < 20; ++i)
            lines.push_back(std::to_string(i + 1) + ". " + mechanics[i].name + ": " + mechanics[i].description.value_or(""));
        mechanics_text = str::join(lines, "\n");
    } else {
        mechanics_text = str::js_slice(mechanics_context, 0, 6000);
        if (mechanics_text.empty()) mechanics_text = theme;
    }
    const std::string system =
        "You create plausible hypothesis-labeled player feedback for game design learning. "
        "Return JSON only. Do not include real personal data or claim these are real reviews.";
    const std::string prompt =
        "Game/theme:\n" + theme + "\n\nMechanics/design notes:\n" + mechanics_text + "\n\n" +
        "Create 12 candidate user voice snippets so the application can keep the most diverse 8. "
        "Use Japanese unless the input is not Japanese. Cover distinct player segments such as first-time player, returning player, experienced player, no-pay/casual player, co-op helper, and monetization-sensitive player. "
        "Cover distinct concerns such as tutorial clarity, early difficulty, reward pacing, onboarding UI, character growth/gacha, social/co-op, and re-engagement. "
        "Balance polarity with at least 4 negative, 3 mixed, and 3 positive candidates. "
        "Do not repeat the same segment+concern pair, conclusion, or sentence frame. "
        "Each item must be {\"segment\":\"...\",\"polarity\":\"positive|negative|mixed\",\"concern\":\"...\",\"content\":\"...\"} and content must start with \"[synthetic] \".";
    try {
        persona_engine::LLMInvokeArgs args;
        args.system = system;
        args.prompt = prompt;
        args.maxTokens = 2500;
        const auto res = llm.invoke(args);
        if (!res.ok) {
            warn("synthetic user voice generation failed: " + res.error);
            return {};
        }
        const auto arr = extract_json_array(res.text);
        if (!arr) return {};
        auto opinions = normalize_synthetic_learning_opinions(*arr, static_cast<double>(time::now_ms()));
        if (opinions.size() < 5) warn("synthetic user voice generation produced only " + std::to_string(opinions.size()) + " diverse items");
        return opinions;
    } catch (const std::exception& e) {
        warn(std::string("synthetic user voice generation threw: ") + e.what());
        return {};
    }
}

} // namespace routes_detail
} // namespace di::flow::web
