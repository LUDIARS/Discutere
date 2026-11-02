#include "crawler/md_format.hpp"

#include "base/strings.hpp"
#include "lib/front_matter/front_matter.hpp"

namespace di::crawler {

namespace {

// SLUG_RE = /^[a-z0-9][a-z0-9-]*$/ (線形)
bool is_slug(std::string_view s) {
    if (s.empty()) return false;
    auto alnum = [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); };
    if (!alnum(s.front())) return false;
    for (char c : s)
        if (!alnum(c) && c != '-') return false;
    return true;
}

std::optional<std::string> opt_str(const Json& e, const char* key) {
    auto it = e.find(key);
    if (it != e.end() && it->is_string()) return it->get<std::string>();
    return std::nullopt;
}

bool nonblank_string(const Json& e, const char* key) {
    auto it = e.find(key);
    return it != e.end() && it->is_string() && !str::trim(it->get<std::string>()).empty();
}

// JS: !entry || typeof entry !== "object" → 配列は object として通る (中身のキーは無い)。
const Json& entry_object(const Json& entry, const char* kind, std::size_t idx) {
    static const Json empty = Json::object();
    if (entry.is_object()) return entry;
    if (entry.is_array()) return empty;
    throw GameKGParseError(std::string(kind) + "[" + std::to_string(idx) + "] is not an object");
}

/** 文字列配列に正規化する (非配列 / 空要素は除去、空配列は undefined)。 */
std::optional<std::vector<std::string>> normalize_string_array(const Json& e, const char* key) {
    auto it = e.find(key);
    if (it == e.end() || !it->is_array()) return std::nullopt;
    std::vector<std::string> out;
    for (const auto& x : *it)
        if (x.is_string() && !str::trim(x.get<std::string>()).empty()) out.push_back(x.get<std::string>());
    if (out.empty()) return std::nullopt;
    return out;
}

std::vector<SourceRef> normalize_sources(const Json& data) {
    std::vector<SourceRef> out;
    auto it = data.find("sources");
    if (it == data.end() || !it->is_array()) return out;
    std::size_t idx = 0;
    for (const auto& entry : *it) {
        const Json& e = entry_object(entry, "sources", idx);
        if (!nonblank_string(e, "url")) throw GameKGParseError("sources[" + std::to_string(idx) + "].url is required");
        std::optional<std::string> policy;
        auto p = e.find("excerpt_policy");
        if (p != e.end() && !p->is_null()) {
            // undefined 以外で 3 値に一致しないものは拒否 (null も JS では undefined ではないので拒否)
            if (!p->is_string() || (*p != "summary-only" && *p != "fair-use-quote" && *p != "forbidden"))
                throw GameKGParseError("sources[" + std::to_string(idx) +
                                       "].excerpt_policy must be one of summary-only / fair-use-quote / forbidden");
            policy = p->get<std::string>();
        } else if (p != e.end() && p->is_null()) {
            throw GameKGParseError("sources[" + std::to_string(idx) +
                                   "].excerpt_policy must be one of summary-only / fair-use-quote / forbidden");
        }
        SourceRef s;
        s.url = e["url"].get<std::string>();
        s.title = opt_str(e, "title");
        s.fetched_at = opt_str(e, "fetched_at");
        s.attribution = opt_str(e, "attribution");
        s.excerpt_policy = policy;
        out.push_back(std::move(s));
        ++idx;
    }
    return out;
}

std::vector<MechanicEntry> normalize_mechanics(const Json& data) {
    std::vector<MechanicEntry> out;
    auto it = data.find("mechanics");
    if (it == data.end() || !it->is_array()) return out;
    std::size_t idx = 0;
    for (const auto& entry : *it) {
        const Json& e = entry_object(entry, "mechanics", idx);
        if (!nonblank_string(e, "name")) throw GameKGParseError("mechanics[" + std::to_string(idx) + "].name is required");
        MechanicEntry m;
        m.name = e["name"].get<std::string>();
        m.description = opt_str(e, "description");
        m.intends = opt_str(e, "intends");
        m.intended_affect = opt_str(e, "intended_affect");
        auto valence = opt_str(e, "intended_valence");
        if (valence && (*valence == "positive" || *valence == "negative" || *valence == "neutral")) m.intended_valence = valence;
        m.intended_aspects = normalize_string_array(e, "intended_aspects");
        m.intended_emotions = normalize_string_array(e, "intended_emotions");
        out.push_back(std::move(m));
        ++idx;
    }
    return out;
}

std::vector<AestheticEntry> normalize_aesthetics(const Json& data) {
    std::vector<AestheticEntry> out;
    auto it = data.find("aesthetics");
    if (it == data.end() || !it->is_array()) return out;
    std::size_t idx = 0;
    for (const auto& entry : *it) {
        const Json& e = entry_object(entry, "aesthetics", idx);
        if (!nonblank_string(e, "name")) throw GameKGParseError("aesthetics[" + std::to_string(idx) + "].name is required");
        AestheticEntry a;
        a.name = e["name"].get<std::string>();
        a.description = opt_str(e, "description");
        out.push_back(std::move(a));
        ++idx;
    }
    return out;
}

} // namespace

GameKG parse_game_kg(std::string_view markdown, const std::optional<std::string>& source_path) {
    std::string error;
    auto matter = lib::front_matter::parse(markdown, &error);
    if (!matter) throw GameKGParseError("frontmatter YAML: " + error, source_path);
    const Json& data = matter->data;

    auto id = data.find("id");
    if (id == data.end() || !id->is_string() || !is_slug(id->get<std::string>())) {
        const std::string got = id == data.end() ? std::string("undefined") : json_stringify(*id);
        throw GameKGParseError("frontmatter.id is required and must be kebab-case ASCII (got " + got + ")", source_path);
    }
    if (!nonblank_string(data, "title")) throw GameKGParseError("frontmatter.title is required", source_path);

    GameKG kg;
    kg.id = id->get<std::string>();
    kg.title = data["title"].get<std::string>();
    kg.genre = opt_str(data, "genre");
    kg.workspace_id = nonblank_string(data, "workspace_id") ? data["workspace_id"].get<std::string>() : std::string(DEFAULT_WORKSPACE);
    kg.sources = normalize_sources(data);
    kg.mechanics = normalize_mechanics(data);
    kg.aesthetics = normalize_aesthetics(data);
    if (!str::trim(matter->content).empty()) kg.body = matter->content;
    return kg;
}

Json game_kg_frontmatter(const GameKG& kg) {
    Json fm = Json::object();
    fm["id"] = kg.id;
    fm["title"] = kg.title;
    if (kg.genre) fm["genre"] = *kg.genre;
    fm["workspace_id"] = kg.workspace_id;
    // to_json は undefined のキーを書かない (= compact)。
    if (!kg.sources.empty()) {
        fm["sources"] = Json::array();
        for (const auto& s : kg.sources) fm["sources"].push_back(to_json(s));
    }
    if (!kg.mechanics.empty()) {
        fm["mechanics"] = Json::array();
        for (const auto& m : kg.mechanics) fm["mechanics"].push_back(to_json(m));
    }
    if (!kg.aesthetics.empty()) {
        fm["aesthetics"] = Json::array();
        for (const auto& a : kg.aesthetics) fm["aesthetics"].push_back(to_json(a));
    }
    return fm;
}

} // namespace di::crawler
