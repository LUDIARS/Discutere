#include "flow/proxy/cast.hpp"

#include <algorithm>
#include <set>

#include "base/strings.hpp"
#include "flow/proxy/bundle.hpp"

namespace di::flow::proxy {
namespace {

bool is_hex_lower(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }

const std::string kLabelHead = "論者#";
const std::string kProxyWord = "代理";
const std::string kGenreWord = "ジャンル";
const std::string kFullColon = "：";

/** s[i..] が /論者#[0-9a-f]{6}/ なら一致の長さ、違えば 0。 */
std::size_t match_label(std::string_view s, std::size_t i) {
    if (s.compare(i, kLabelHead.size(), kLabelHead) != 0) return 0;
    const std::size_t h = i + kLabelHead.size();
    if (h + 6 > s.size()) return 0;
    for (std::size_t k = 0; k < 6; ++k)
        if (!is_hex_lower(s[h + k])) return 0;
    return kLabelHead.size() + 6;
}

/** \s* を読み飛ばす (JS の空白)。 */
std::size_t skip_spaces(std::string_view s, std::size_t i) {
    while (i < s.size()) {
        std::size_t j = i;
        const auto cp = str::next_codepoint(s, j);
        if (!str::is_js_space(cp)) break;
        i = j;
    }
    return i;
}

/** [:：] を 1 つ読む。読めなければ npos。 */
std::size_t colon(std::string_view s, std::size_t i, bool allow_equal = false) {
    if (i < s.size() && (s[i] == ':' || (allow_equal && s[i] == '='))) return i + 1;
    if (s.compare(i, kFullColon.size(), kFullColon) == 0) return i + kFullColon.size();
    return std::string_view::npos;
}

/** [\s,、と&vsVS] の 1 文字か。 */
bool is_separator(std::uint32_t cp) {
    return str::is_js_space(cp) || cp == ',' || cp == 0x3001 /* 、 */ || cp == 0x3068 /* と */ || cp == '&' || cp == 'v' ||
           cp == 's' || cp == 'V' || cp == 'S';
}

struct Found {
    std::size_t start = 0, end = 0, group_start = 0, group_end = 0;
};

/** /代理\s*[:：]\s*((?:論者#[0-9a-f]{6}[\s,、と&vsVS]*)+)/ の最初の一致。 */
std::optional<Found> find_directive(std::string_view s) {
    for (std::size_t pos = s.find(kProxyWord); pos != std::string_view::npos; pos = s.find(kProxyWord, pos + 1)) {
        std::size_t i = skip_spaces(s, pos + kProxyWord.size());
        i = colon(s, i);
        if (i == std::string_view::npos) continue;
        i = skip_spaces(s, i);
        const std::size_t group_start = i;
        std::size_t n = match_label(s, i);
        if (n == 0) continue;
        while (n > 0) {
            i += n;
            while (i < s.size()) {
                std::size_t j = i;
                if (!is_separator(str::next_codepoint(s, j))) break;
                i = j;
            }
            n = match_label(s, i);
        }
        return Found{pos, i, group_start, i};
    }
    return std::nullopt;
}

bool is_genre_head(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); }
bool is_genre_tail(char c) { return is_genre_head(c) || c == '_' || c == '-'; }

/** /ジャンル\s*[:：]\s*([a-z0-9][a-z0-9_-]*)/ の最初の一致。 */
std::optional<Found> find_genre(std::string_view s) {
    for (std::size_t pos = s.find(kGenreWord); pos != std::string_view::npos; pos = s.find(kGenreWord, pos + 1)) {
        std::size_t i = skip_spaces(s, pos + kGenreWord.size());
        i = colon(s, i);
        if (i == std::string_view::npos) continue;
        i = skip_spaces(s, i);
        if (i >= s.size() || !is_genre_head(s[i])) continue;
        const std::size_t g = i;
        ++i;
        while (i < s.size() && is_genre_tail(s[i])) ++i;
        return Found{pos, i, g, i};
    }
    return std::nullopt;
}

/** s.replace(needle, "") (最初の 1 つ)。 */
std::string replace_first(const std::string& s, const std::string& needle) {
    if (needle.empty()) return s;
    const auto p = s.find(needle);
    return p == std::string::npos ? s : s.substr(0, p) + s.substr(p + needle.size());
}

/** [\s/|・,、] */
bool is_trim_char(std::uint32_t cp) {
    return str::is_js_space(cp) || cp == '/' || cp == '|' || cp == 0x30FB /* ・ */ || cp == ',' || cp == 0x3001 /* 、 */;
}

/** .replace(/^[\s/|・,、]+|[\s/|・,、]+$/g, "") */
std::string trim_chars(const std::string& s) {
    const auto cps = str::codepoints(s);
    std::size_t b = 0, e = cps.size();
    while (b < e && is_trim_char(cps[b])) ++b;
    while (e > b && is_trim_char(cps[e - 1])) --e;
    return str::from_codepoints(std::vector<std::uint32_t>(cps.begin() + static_cast<std::ptrdiff_t>(b),
                                                           cps.begin() + static_cast<std::ptrdiff_t>(e)));
}

} // namespace

Json to_json(const ProxyRequest& r) {
    Json labels = Json::array();
    for (const auto& l : r.labels) labels.push_back(l);
    return Json{{"labels", labels}, {"genre", r.genre ? Json(*r.genre) : Json(nullptr)}, {"theme", r.theme}};
}

std::optional<ProxyRequest> parse_proxy_request(const std::string& text) {
    const auto m = find_directive(text);
    if (!m) return std::nullopt;
    ProxyRequest out;
    const std::string_view group = std::string_view(text).substr(m->group_start, m->group_end - m->group_start);
    std::set<std::string> seen;
    for (std::size_t i = 0; i < group.size();) {
        const std::size_t n = match_label(group, i);
        if (n > 0) {
            std::string label(group.substr(i, n));
            if (seen.insert(label).second) out.labels.push_back(label);
            i += n;
        } else {
            ++i;
        }
    }
    const auto g = find_genre(text);
    std::string theme = replace_first(text, text.substr(m->start, m->end - m->start));
    if (g) {
        out.genre = text.substr(g->group_start, g->group_end - g->group_start);
        theme = replace_first(theme, text.substr(g->start, g->end - g->start));
    }
    theme = str::trim(trim_chars(theme));
    out.theme = theme.empty() ? str::trim(text) : theme;
    return out;
}

std::string choose_genre(const std::vector<std::vector<ProxyProfileRow>>& rows_by_label, const std::optional<std::string>& requested) {
    if (requested && !requested->empty()) return *requested;
    std::vector<std::set<std::string>> sets;
    for (const auto& rows : rows_by_label) {
        std::set<std::string> s;
        for (const auto& r : rows) s.insert(r.genre);
        sets.push_back(std::move(s));
    }
    // [...sets[0]] は挿入順。rows_by_label[0] の順で重複なく並べる。
    std::vector<std::string> common;
    if (!rows_by_label.empty()) {
        std::set<std::string> added;
        for (const auto& r : rows_by_label[0]) {
            if (!added.insert(r.genre).second) continue;
            if (std::all_of(sets.begin(), sets.end(), [&](const auto& s) { return s.count(r.genre) > 0; })) common.push_back(r.genre);
        }
    }
    if (common.size() == 1) return common[0];
    if (common.empty())
        throw ProxyRequestError("2 人に共通するジャンルの人物像がありません。「ジャンル: <id>」で指定してください。");
    throw ProxyRequestError("共通するジャンルが複数あります (" + str::join(common, ", ") + ")。「ジャンル: <id>」で指定してください。");
}

std::vector<FlowPersona> load_proxy_participants(const LoadProxyParticipantsArgs& args) {
    const auto& request = args.request;
    if (request.labels.size() != 2)
        throw ProxyRequestError("代理議論は 2 人で行います (指定 " + std::to_string(request.labels.size()) +
                                " 人)。「代理: 論者#xxxxxx, 論者#yyyyyy」と書いてください。");
    std::vector<std::vector<ProxyProfileRow>> rows_by_label;
    for (const auto& label : request.labels) {
        auto rows = find_proxy_profiles_by_label(label);
        if (rows.empty()) throw ProxyRequestError(label + " の人物像が取り込まれていません。");
        rows_by_label.push_back(std::move(rows));
    }
    const std::string genre = choose_genre(rows_by_label, request.genre);
    const std::string budget = js_number_to_string(args.budgetTokens);

    std::vector<FlowPersona> out;
    for (std::size_t i = 0; i < request.labels.size(); ++i) {
        const auto& label = request.labels[i];
        const auto& rows = rows_by_label[i];
        auto it = std::find_if(rows.begin(), rows.end(), [&](const ProxyProfileRow& r) { return r.genre == genre; });
        if (it == rows.end()) throw ProxyRequestError(label + " にジャンル " + genre + " の人物像がありません。");
        const auto& row = *it;
        if (auto blocked = is_castable(row, args.now, args.requirePassed))
            throw ProxyRequestError(label + " (" + genre + ") は代理に出せません: " + *blocked + "。");
        const auto bundle = render_proxy_bundle(row.pack, {args.budgetTokens, args.focusTerms, label});
        if (!bundle) throw ProxyRequestError(label + " (" + genre + ") の人物像を予算 " + budget + " トークンに収められません。");
        std::string model;
        if (i < args.models.size()) model = args.models[i];
        else if (!args.models.empty()) model = args.models.back();
        if (model.empty()) throw ProxyRequestError("代理に割り当てるモデルがありません。");
        FlowPersona persona;
        persona.id = "proxy:" + row.speakerKey + ":" + genre;
        persona.name = label;
        persona.role = "debater";
        persona.stance = "pro";
        persona.speechStyle = !row.pack.styleNotes.empty() ? str::join(row.pack.styleNotes, " / ") : "本人の発言に近い口語";
        persona.traits = {"公開レビュー由来の AI 代理", "ジャンル " + genre};
        persona.model = model;
        persona.isLocal = args.isLocal;
        persona.possession = PersonaPossession{PROXY_POSSESSION_LABEL, std::nullopt};
        ProxyPersonaContext ctx;
        ctx.label = label;
        ctx.speakerKey = row.speakerKey;
        ctx.genre = genre;
        ctx.bundleText = bundle->text;
        ctx.evidenceIds = bundle->evidenceIds;
        ctx.evidenceTexts = bundle->evidenceTexts;
        persona.proxy = std::move(ctx);
        out.push_back(std::move(persona));
    }
    return out;
}

} // namespace di::flow::proxy
