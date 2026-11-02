#include "visualize/learning_layers.hpp"

#include <algorithm>
#include <vector>

#include "base/fsutil.hpp"
#include "base/strings.hpp"
#include "base/time.hpp"
#include "visualize/locale_compare.hpp"
#include "visualize/topic_opinions.hpp"

namespace di::visualize {
namespace {

std::int64_t count(sql::Database& db, const char* table, const std::string& workspace_id) {
    // table は固定の名前だけ (利用者の入力ではない)。
    auto row = db.get(std::string("SELECT COUNT(*) AS c FROM ") + table + " WHERE workspace_id = ?", {workspace_id});
    return row ? row->i64("c") : 0;
}

Json string_or_null(const Json& entry, const char* key) {
    if (!entry.is_object()) return nullptr;
    auto it = entry.find(key);
    if (it == entry.end() || !it->is_string()) return nullptr;
    const std::string t = str::trim(it->get_ref<const std::string&>());
    return t.empty() ? Json(nullptr) : Json(t);
}

std::vector<Json> read_affect_vocabulary(const std::string& path) {
    auto text = fs::read_file(path, kAffectVocabularyMaxBytes);
    if (!text) return {};
    auto parsed = parse_json(*text, JsonLimits{kAffectVocabularyMaxBytes, 200'000, 32});
    if (!parsed || !parsed->is_object()) return {};
    auto it = parsed->find("affects");
    if (it == parsed->end() || !it->is_array()) return {};
    std::vector<Json> out;
    for (const auto& entry : *it) {
        const Json key = string_or_null(entry, "key");
        if (key.is_null()) continue;
        Json j = Json::object();
        j["key"] = key;
        j["labelJa"] = string_or_null(entry, "label_ja");
        j["valence"] = string_or_null(entry, "valence");
        j["mda"] = string_or_null(entry, "mda");
        j["description"] = string_or_null(entry, "description");
        j["usageCount"] = 0;
        j["size"] = 1;
        out.push_back(std::move(j));
    }
    return out;
}

Json knowledge_layer(sql::Database& db, const std::string& ws, const LearningLayerOptions& options) {
    const std::int64_t limit = clamp_limit(options.limit.value_or(80), 1, 300);
    const std::string path = fs::resolve(options.vocabularyPath.empty() ? "data/affects/vocabulary.json" : options.vocabularyPath);
    auto vocabulary = read_affect_vocabulary(path);
    const std::size_t vocabulary_size = vocabulary.size();
    for (auto& affect : vocabulary) {
        const std::string key = affect["key"].get<std::string>();
        auto row = db.get(
            "SELECT\n"
            "       (SELECT COUNT(*) FROM mechanics WHERE workspace_id = ? AND intended_affect = ?) +\n"
            "       (SELECT COUNT(*) FROM affects WHERE workspace_id = ? AND mood = ?) +\n"
            "       (SELECT COUNT(*) FROM design_gaps WHERE workspace_id = ? AND (expected_affect = ? OR observed_affect = ?)) AS c",
            {ws, key, ws, key, ws, key, key});
        const std::int64_t usage = row ? row->i64("c") : 0;
        affect["usageCount"] = usage;
        affect["size"] = std::max<std::int64_t>(1, usage);
    }
    std::stable_sort(vocabulary.begin(), vocabulary.end(), [](const Json& a, const Json& b) {
        const auto sa = a["size"].get<std::int64_t>();
        const auto sb = b["size"].get<std::int64_t>();
        if (sa != sb) return sb < sa;
        return locale_compare(a["key"].get<std::string>(), b["key"].get<std::string>()) < 0;
    });
    if (vocabulary.size() > static_cast<std::size_t>(limit)) vocabulary.resize(static_cast<std::size_t>(limit));

    Json out = Json::object();
    out["layer"] = "knowledge";
    out["workspaceId"] = ws;
    out["generatedAt"] = time::now_ms();
    out["totals"] = Json{{"affects", vocabulary_size},
                         {"mechanics", count(db, "mechanics", ws)},
                         {"aesthetics", count(db, "aesthetics", ws)},
                         {"games", count(db, "games", ws)},
                         {"gaps", count(db, "design_gaps", ws)},
                         {"hypotheses", count(db, "hypotheses", ws)}};
    Json affects = Json::array();
    for (auto& a : vocabulary) affects.push_back(std::move(a));
    out["affects"] = affects;
    return out;
}

Json games_layer(sql::Database& db, const std::string& ws, const LearningLayerOptions& options) {
    const std::int64_t limit = clamp_limit(options.limit.value_or(50), 1, 200);
    const std::int64_t detail = clamp_limit(options.detailLimit.value_or(8), 1, 50);
    const auto games = db.all(
        "SELECT id, title, genre, updated_at\n"
        "         FROM games\n"
        "        WHERE workspace_id = ?\n"
        "        ORDER BY updated_at DESC\n"
        "        LIMIT ?",
        {ws, limit});
    struct Entry {
        Json json;
        std::int64_t size;
        double updated_at;
        std::int64_t mechanics, aesthetics, opinions;
    };
    std::vector<Entry> entries;
    for (const auto& game : games) {
        const std::string id = game.str("id");
        auto c1 = db.get("SELECT COUNT(*) AS c FROM mechanics WHERE workspace_id = ? AND game_id = ?", {ws, id});
        auto c2 = db.get("SELECT COUNT(*) AS c FROM aesthetics WHERE workspace_id = ? AND game_id = ?", {ws, id});
        auto c3 = db.get(
            "SELECT COUNT(*) AS c\n"
            "       FROM hypotheses h\n"
            "       JOIN design_gaps g ON g.id = h.design_gap_id AND g.workspace_id = h.workspace_id\n"
            "      WHERE h.workspace_id = ? AND g.game_id = ?",
            {ws, id});
        const std::int64_t mechanic_count = c1 ? c1->i64("c") : 0;
        const std::int64_t aesthetic_count = c2 ? c2->i64("c") : 0;
        const std::int64_t opinion_count = c3 ? c3->i64("c") : 0;
        Json mechanics = Json::array();
        for (const auto& m : db.all(
                 "SELECT id, name, description, intends, intended_affect\n"
                 "       FROM mechanics\n"
                 "      WHERE workspace_id = ? AND game_id = ?\n"
                 "      ORDER BY updated_at DESC\n"
                 "      LIMIT ?",
                 {ws, id, detail})) {
            Json j = Json::object();
            j["id"] = m["id"].to_json();
            j["name"] = m["name"].to_json();
            j["description"] = m["description"].to_json();
            j["intends"] = m["intends"].to_json();
            j["intendedAffect"] = m["intended_affect"].to_json();
            mechanics.push_back(std::move(j));
        }
        Json aesthetics = Json::array();
        for (const auto& a : db.all(
                 "SELECT id, name, description\n"
                 "       FROM aesthetics\n"
                 "      WHERE workspace_id = ? AND game_id = ?\n"
                 "      ORDER BY updated_at DESC\n"
                 "      LIMIT ?",
                 {ws, id, detail}))
            aesthetics.push_back(a.to_json());
        const std::int64_t size = mechanic_count * 2 + aesthetic_count + opinion_count * 3;
        Json j = Json::object();
        j["id"] = game["id"].to_json();
        j["title"] = game["title"].to_json();
        j["genre"] = game["genre"].to_json();
        j["mechanicCount"] = mechanic_count;
        j["aestheticCount"] = aesthetic_count;
        j["opinionCount"] = opinion_count;
        j["size"] = size;
        j["updatedAt"] = game["updated_at"].to_json();
        j["mechanics"] = mechanics;
        j["aesthetics"] = aesthetics;
        entries.push_back({std::move(j), size, game["updated_at"].as_double(0), mechanic_count, aesthetic_count, opinion_count});
    }
    std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.size != b.size) return b.size < a.size;
        return b.updated_at < a.updated_at;
    });
    std::int64_t m = 0, a = 0, o = 0;
    Json list = Json::array();
    for (auto& e : entries) {
        m += e.mechanics;
        a += e.aesthetics;
        o += e.opinions;
        list.push_back(std::move(e.json));
    }
    Json out = Json::object();
    out["layer"] = "games";
    out["workspaceId"] = ws;
    out["generatedAt"] = time::now_ms();
    out["totals"] = Json{{"games", entries.size()}, {"mechanics", m}, {"aesthetics", a}, {"opinions", o}};
    out["games"] = list;
    return out;
}

} // namespace

Json build_learning_layer_snapshot(sql::Database& db, const std::string& workspace_id, const LearningLayer& layer,
                                   const LearningLayerOptions& options) {
    if (layer == "knowledge") return knowledge_layer(db, workspace_id, options);
    if (layer == "games") return games_layer(db, workspace_id, options);
    TopicOpinionOptions topic_options;
    topic_options.limit = options.limit;
    topic_options.opinionsPerTopic = options.detailLimit;
    const Json snap = build_topic_opinion_snapshot(db, workspace_id, topic_options);
    Json out = Json::object();
    out["layer"] = "opinions";
    for (auto it = snap.begin(); it != snap.end(); ++it) out[it.key()] = it.value();
    return out;
}

LearningLayer normalize_learning_layer(const std::optional<std::string>& value) {
    if (value && (*value == "knowledge" || *value == "games" || *value == "opinions")) return *value;
    return "knowledge";
}

} // namespace di::visualize
