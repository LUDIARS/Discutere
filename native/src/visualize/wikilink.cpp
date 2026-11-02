#include "visualize/wikilink.hpp"

#include <stdexcept>

#include "base/strings.hpp"

namespace di::visualize {
namespace {

bool is_type_char(char c) { return c >= 'a' && c <= 'z'; }

/** [^\[\]\s] (1 文字。JS の \s は code point で判定)。 */
bool is_id_cp(std::uint32_t cp) { return cp != '[' && cp != ']' && !str::is_js_space(cp); }

/** /^[^\s\[\]]+$/ */
bool is_valid_id(std::string_view id) {
    if (id.empty()) return false;
    for (std::uint32_t cp : str::codepoints(id))
        if (!is_id_cp(cp)) return false;
    return true;
}

/**
 * text[pos..] が `[[type:id]]` で始まるかを見る。一致すれば終わりの位置 (直後) を返す。
 * 正規表現 /\[\[([a-z]+):([^\[\]\s]+)\]\]/ と同じ (id は ']' で止まるので後戻りは要らない)。
 */
std::optional<std::size_t> match_at(std::string_view text, std::size_t pos, Wikilink* out) {
    if (pos + 1 >= text.size() || text[pos] != '[' || text[pos + 1] != '[') return std::nullopt;
    std::size_t i = pos + 2;
    const std::size_t type_begin = i;
    while (i < text.size() && is_type_char(text[i])) ++i;
    if (i == type_begin || i >= text.size() || text[i] != ':') return std::nullopt;
    const std::size_t type_end = i;
    ++i;
    const std::size_t id_begin = i;
    while (i < text.size()) {
        std::size_t next = i;
        const std::uint32_t cp = str::next_codepoint(text, next);
        if (!is_id_cp(cp)) break;
        i = next;
    }
    if (i == id_begin || i + 1 >= text.size() || text[i] != ']' || text[i + 1] != ']') return std::nullopt;
    if (out) {
        out->type = std::string(text.substr(type_begin, type_end - type_begin));
        out->id = std::string(text.substr(id_begin, i - id_begin));
    }
    return i + 2;
}

} // namespace

const std::vector<std::pair<std::string, std::string>> TYPE_TO_TABLE{
    {"utt", "utterances"}, {"hyp", "hypotheses"}, {"gap", "design_gaps"}, {"mch", "mechanics"}, {"aes", "aesthetics"},
    {"aff", "affects"},    {"ses", "sessions"},   {"game", "games"},      {"persona", "personas"},
};

const std::vector<std::pair<std::string, std::string>> TYPE_TO_DIR{
    {"utt", "utterance"}, {"hyp", "hypothesis"}, {"gap", "gap"},   {"mch", "mechanic"},   {"aes", "aesthetic"},
    {"aff", "affect"},    {"ses", "session"},    {"game", "game"}, {"persona", "persona"},
};

const std::vector<std::pair<std::string, std::string>> FULL_TO_PREFIX{
    {"utterance", "utt"}, {"hypothesis", "hyp"}, {"gap", "gap"},     {"designgap", "gap"}, {"design-gap", "gap"},
    {"mechanic", "mch"},  {"aesthetic", "aes"},  {"affect", "aff"},  {"session", "ses"},   {"game", "game"},
    {"persona", "persona"},
};

std::optional<std::string> lookup(const std::vector<std::pair<std::string, std::string>>& table, std::string_view key) {
    for (const auto& [k, v] : table)
        if (k == key) return v;
    return std::nullopt;
}

Json to_json(const Wikilink& link) { return Json{{"type", link.type}, {"id", link.id}}; }

bool is_node_type(std::string_view value) {
    for (auto t : NODE_TYPES)
        if (t == value) return true;
    return false;
}

std::optional<Wikilink> parse_wikilink(std::string_view text) {
    const std::string trimmed = str::trim(text);
    Wikilink link;
    auto end = match_at(trimmed, 0, &link);
    if (!end || *end != trimmed.size()) return std::nullopt;
    if (!is_node_type(link.type)) return std::nullopt;
    if (link.id.empty()) return std::nullopt;
    return link;
}

std::string format_wikilink(const Wikilink& link) {
    if (!is_node_type(link.type)) throw std::invalid_argument("unknown node type: " + link.type);
    if (!is_valid_id(link.id))
        throw std::invalid_argument("invalid id (must not contain whitespace or brackets): " + link.id);
    return "[[" + link.type + ":" + link.id + "]]";
}

std::vector<Wikilink> extract_wikilinks(std::string_view text) {
    std::vector<Wikilink> out;
    std::size_t pos = 0;
    while (pos < text.size()) {
        Wikilink link;
        if (auto end = match_at(text, pos, &link)) {
            if (is_node_type(link.type) && is_valid_id(link.id)) out.push_back(std::move(link));
            pos = *end;
        } else {
            ++pos;
        }
    }
    return out;
}

std::optional<std::string> normalize_type(std::string_view input) {
    const std::string lower = str::to_lower(input);
    if (is_node_type(lower)) return lower;
    return lookup(FULL_TO_PREFIX, lower);
}

} // namespace di::visualize
