#include "crawler/types.hpp"

namespace di::crawler {

namespace {
void put(Json& o, const char* key, const std::optional<std::string>& v) {
    if (v) o[key] = *v;
}
} // namespace

Json to_json(const SourceRef& v) {
    Json o = Json::object();
    o["url"] = v.url;
    put(o, "title", v.title);
    put(o, "fetched_at", v.fetched_at);
    put(o, "attribution", v.attribution);
    put(o, "excerpt_policy", v.excerpt_policy);
    return o;
}

Json to_json(const MechanicEntry& v) {
    Json o = Json::object();
    o["name"] = v.name;
    put(o, "description", v.description);
    put(o, "intends", v.intends);
    put(o, "intended_affect", v.intended_affect);
    put(o, "intended_valence", v.intended_valence);
    if (v.intended_aspects) o["intended_aspects"] = *v.intended_aspects;
    if (v.intended_emotions) o["intended_emotions"] = *v.intended_emotions;
    return o;
}

Json to_json(const AestheticEntry& v) {
    Json o = Json::object();
    o["name"] = v.name;
    put(o, "description", v.description);
    return o;
}

Json to_json(const GameKG& v) {
    Json o = Json::object();
    o["id"] = v.id;
    o["title"] = v.title;
    put(o, "genre", v.genre);
    o["workspace_id"] = v.workspace_id;
    o["sources"] = Json::array();
    for (const auto& s : v.sources) o["sources"].push_back(to_json(s));
    o["mechanics"] = Json::array();
    for (const auto& m : v.mechanics) o["mechanics"].push_back(to_json(m));
    o["aesthetics"] = Json::array();
    for (const auto& a : v.aesthetics) o["aesthetics"].push_back(to_json(a));
    put(o, "body", v.body);
    return o;
}

} // namespace di::crawler
