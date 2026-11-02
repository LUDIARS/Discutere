#include "flow/investigate.hpp"

#include <set>

#include "base/fsutil.hpp"
#include "base/log.hpp"
#include "base/strings.hpp"
#include "lib/front_matter/front_matter.hpp"

namespace di::flow {

namespace {

bool is_search_noise(std::uint32_t cp) {
    // [・\s_\-:：()（）「」『』"']
    return cp == U'・' || cp == '_' || cp == '-' || cp == ':' || cp == U'：' || cp == '(' || cp == ')' || cp == U'（' ||
           cp == U'）' || cp == U'「' || cp == U'」' || cp == U'『' || cp == U'』' || cp == '"' || cp == '\'' ||
           str::is_js_space(cp);
}

std::string normalize_search_text(std::string_view value) {
    const std::string lower = str::to_lower(value);
    std::string out;
    std::size_t i = 0;
    while (i < lower.size()) {
        const std::size_t start = i;
        if (!is_search_noise(str::next_codepoint(lower, i))) out.append(lower, start, i - start);
    }
    return str::trim(out);
}

std::vector<std::string> theme_tokens(std::string_view theme) {
    // theme.toLowerCase().split(/[\s　・_\-:：()（）「」『』"']+/) (区切り文字は normalize と同じ集合)
    const std::string lower = str::to_lower(theme);
    std::vector<std::string> tokens;
    std::string current;
    std::size_t i = 0;
    auto push = [&] {
        const std::string t = str::trim(current);
        if (str::js_length(t) >= 2) tokens.push_back(t);
        current.clear();
    };
    while (i < lower.size()) {
        const std::size_t start = i;
        if (is_search_noise(str::next_codepoint(lower, i))) push();
        else current.append(lower, start, i - start);
    }
    push();
    const std::string compact = normalize_search_text(theme);
    if (str::js_length(compact) >= 2) tokens.push_back(compact);
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (auto& t : tokens)
        if (seen.insert(t).second) out.push_back(t);
    return out;
}

std::string join_strings(const Json& o, std::initializer_list<const char*> keys, const std::string* first = nullptr) {
    std::vector<std::string> parts;
    if (first) parts.push_back(*first);
    for (const char* k : keys)
        if (o.is_object() && o.contains(k) && o[k].is_string()) parts.push_back(o[k].get<std::string>());
    return str::join(parts, " ");
}

bool any_token_in(const std::vector<std::string>& tokens, const std::string& haystack) {
    for (const auto& t : tokens)
        if (haystack.find(normalize_search_text(t)) != std::string::npos) return true;
    return false;
}

bool file_matches_theme(const std::string& file, const Json& data, const std::vector<std::string>& tokens) {
    if (tokens.empty()) return false;
    std::string stem = file;
    if (stem.size() >= 3 && str::to_lower_ascii(stem.substr(stem.size() - 3)) == ".md") stem.resize(stem.size() - 3);
    return any_token_in(tokens, normalize_search_text(join_strings(data, {"id", "title", "genre"}, &stem)));
}

bool mechanic_matches_theme(const Json& mechanics, const std::vector<std::string>& tokens) {
    if (tokens.empty()) return false;
    for (const auto& item : mechanics)
        if (any_token_in(tokens, normalize_search_text(join_strings(item, {"name", "description", "intended_affect"}))))
            return true;
    return false;
}

std::optional<std::vector<std::string>> string_list(const Json& item, const char* key) {
    if (!item.contains(key) || !item[key].is_array()) return std::nullopt;
    std::vector<std::string> out;
    for (const auto& x : item[key])
        if (x.is_string()) out.push_back(x.get<std::string>());
    return out;
}

double default_get_sentiment_count(const std::string& games_dir, const std::string& theme) {
    if (!fs::is_dir(games_dir)) return 0;
    const std::string lower_theme = str::to_lower(theme);
    std::vector<std::string> words;
    for (const auto& w : str::split(str::collapse_whitespace(lower_theme), " "))
        if (str::js_length(w) >= 2) words.push_back(w);
    double count = 0;
    for (const auto& f : fs::list_dir(games_dir)) {
        if (!str::ends_with(f, ".sentiment.json")) continue;
        const std::string lf = str::to_lower(f);
        bool hit = false;
        for (const auto& w : words) hit = hit || lf.find(w) != std::string::npos;
        if (!hit) continue;
        // パース失敗 (上限超過を含む) は無視
        auto text = fs::read_file(fs::join(games_dir, f), kMaxSentimentFileBytes);
        if (!text) continue;
        auto data = parse_json(*text, {kMaxSentimentFileBytes, 5'000'000, 64});
        if (!data) continue;
        if (data->is_array()) count += static_cast<double>(data->size());
        else if (data->is_object() && data->contains("comments") && (*data)["comments"].is_array())
            count += static_cast<double>((*data)["comments"].size());
    }
    return count;
}

} // namespace

std::optional<MechanicSource> as_mechanic_source(const Json& v) {
    if (!v.is_string()) return std::nullopt;
    const auto& s = v.get_ref<const std::string&>();
    if (s == "curated" || s == "llm" || s == "crawl") return s;
    return std::nullopt;
}

Json to_json(const MechanicSummary& m) {
    Json o = Json::object();
    o["name"] = m.name;
    o["description"] = m.description;
    if (m.source) o["source"] = *m.source;
    if (m.intended_affect) o["intended_affect"] = *m.intended_affect;
    if (m.intended_valence) o["intended_valence"] = *m.intended_valence;
    if (m.intended_aspects) o["intended_aspects"] = *m.intended_aspects;
    if (m.intended_emotions) o["intended_emotions"] = *m.intended_emotions;
    return o;
}

MechanicSummary mechanic_summary_from_json(const Json& j) {
    MechanicSummary m;
    m.name = json_string(j, "name");
    m.description = json_string(j, "description");
    m.intended_affect = json_opt_string(j, "intended_affect");
    m.intended_valence = json_opt_string(j, "intended_valence");
    m.intended_aspects = string_list(j, "intended_aspects");
    m.intended_emotions = string_list(j, "intended_emotions");
    if (j.is_object() && j.contains("source")) m.source = as_mechanic_source(j["source"]);
    return m;
}

std::vector<MechanicSummary> load_theme_mechanics(const std::string& games_dir, const std::string& theme,
                                                  const std::function<void(const std::string&)>& warn) {
    if (!fs::is_dir(games_dir)) return {};
    const auto tokens = theme_tokens(theme);
    std::vector<MechanicSummary> results;
    for (const auto& file : fs::list_dir(games_dir)) {
        if (!str::ends_with(file, ".md") || str::contains(file, ".sentiment")) continue;
        auto content = fs::read_file(fs::join(games_dir, file), kMaxGameFileBytes);
        if (!content) {
            if (warn) warn("ゲームデータを読めません (大きすぎるか読めない): " + file);
            continue;
        }
        std::string error;
        auto matter = lib::front_matter::parse(*content, &error);
        if (!matter) {
            if (warn) warn("front matter を読めません: " + file + ": " + error);
            continue;
        }
        const Json& data = matter->data;
        const Json& mechanics = json_array(data, "mechanics");
        if (mechanics.empty()) continue;
        if (!file_matches_theme(file, data, tokens) && !mechanic_matches_theme(mechanics, tokens)) continue;
        for (const auto& item : mechanics) {
            const std::string name = json_string(item, "name");
            if (name.empty()) continue;
            MechanicSummary m;
            m.name = name;
            m.description = json_string(item, "description");
            // 出所: frontmatter に source があれば尊重 (learning が crawl と書く)、無ければ curated。
            m.source = item.is_object() && item.contains("source") ? as_mechanic_source(item["source"]) : std::nullopt;
            if (!m.source) m.source = "curated";
            m.intended_affect = json_opt_string(item, "intended_affect");
            m.intended_valence = json_opt_string(item, "intended_valence");
            m.intended_aspects = string_list(item, "intended_aspects");
            m.intended_emotions = string_list(item, "intended_emotions");
            results.push_back(std::move(m));
        }
    }
    if (results.size() > 10) results.resize(10);
    return results;
}

InvestigateResult investigate_theme(const InvestigateArgs& args) {
    const auto warn = args.warn ? args.warn : [](const std::string& m) { log::warn("[investigate] " + m); };
    InvestigateResult r;
    r.mechanics = load_theme_mechanics(args.gamesDir, args.theme, warn);
    r.sentimentCount = args.getSentimentCount ? args.getSentimentCount(args.theme)
                                              : default_get_sentiment_count(args.gamesDir, args.theme);
    // YouTube 補完条件: 感情データ 0 件 + 外部収集 OK + youtubeSearch 実装あり
    if (r.sentimentCount == 0 && can_collect_external(args.tags) && args.youtubeSearch) {
        warn("感情データが見つかりません (テーマ: \"" + args.theme + "\")。YouTube でコメントを検索します。 (最大 " +
             std::to_string(args.youtubeMaxComments) + " 件)");
        try {
            r.youtubeComments = args.youtubeSearch(args.theme, args.youtubeMaxComments);
            r.youtubeUsed = true;
        } catch (const std::exception& e) {
            warn(std::string("YouTube 検索に失敗しました: ") + e.what());
        }
    }
    return r;
}

} // namespace di::flow
