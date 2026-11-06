#include "flow/games_md.hpp"

#include <map>
#include <stdexcept>

#include "base/fsutil.hpp"
#include "lib/front_matter/front_matter.hpp"

namespace di::flow {

Json to_json(const GameMechanicEntry& e) {
    Json o = Json::object();
    o["name"] = e.name;
    if (e.description) o["description"] = *e.description;
    if (e.intended_affect) o["intended_affect"] = *e.intended_affect;
    if (e.intended_valence) o["intended_valence"] = *e.intended_valence;
    if (e.intended_aspects) o["intended_aspects"] = *e.intended_aspects;
    if (e.intended_emotions) o["intended_emotions"] = *e.intended_emotions;
    if (e.source) o["source"] = *e.source;
    return o;
}

int upsert_game_mechanics(const UpsertGameMechanicsArgs& args) {
    if (args.mechanics.empty()) return 0;

    if (!fs::exists(args.gamesDir)) fs::ensure_dir(args.gamesDir);
    // slug が gamesDir の外を指さないことを確かめる (移植前は検査なし)。
    auto file_path = fs::safe_join(args.gamesDir, args.slug + ".md");
    if (!file_path) throw std::runtime_error("invalid game slug: " + args.slug);

    Json data = Json::object();
    std::string body;
    if (fs::exists(*file_path)) {
        auto text = fs::read_file(*file_path, kMaxGameFileBytes);
        if (!text) throw std::runtime_error("cannot read game file (too large or unreadable): " + *file_path);
        std::string error;
        auto parsed = lib::front_matter::parse(*text, &error);
        if (!parsed) throw std::runtime_error("invalid front matter in " + *file_path + ": " + error);
        data = parsed->data.is_object() ? parsed->data : Json::object();
        body = parsed->content;
    } else {
        data = Json::object();
        data["id"] = args.slug;
        data["title"] = args.title;
        body = "# " + args.title + "\n\n学習フローで収集したメカニクス。\n";
    }

    // name → entry (Map 相当。挿入順を保つ)。name の型も区別する (JSON 表現をキーにする)。
    std::vector<Json> entries;
    std::map<std::string, std::size_t> by_name;
    auto key_of = [](const Json& entry) {
        if (entry.is_object()) {
            auto it = entry.find("name");
            if (it != entry.end()) return json_stringify(*it);
        }
        return std::string("undefined");
    };
    const auto existing_it = data.find("mechanics");
    if (existing_it != data.end() && existing_it->is_array()) {
        for (const auto& m : *existing_it) {
            const std::string key = key_of(m);
            auto found = by_name.find(key);
            if (found != by_name.end()) entries[found->second] = m;
            else {
                by_name.emplace(key, entries.size());
                entries.push_back(m);
            }
        }
    }

    int count = 0;
    for (const auto& m : args.mechanics) {
        if (m.name.empty()) continue;
        ++count;
        // 未定義キーを混ぜないよう定義済みプロパティのみで構築し、既存エントリへ重ねる ({...old, ...entry})。
        const Json entry = to_json(m);
        const std::string key = json_stringify(Json(m.name));
        auto found = by_name.find(key);
        if (found != by_name.end()) {
            Json merged = entries[found->second].is_object() ? entries[found->second] : Json::object();
            for (auto it = entry.begin(); it != entry.end(); ++it) merged[it.key()] = it.value();
            entries[found->second] = std::move(merged);
        } else {
            by_name.emplace(key, entries.size());
            entries.push_back(entry);
        }
    }

    Json list = Json::array();
    for (auto& e : entries) list.push_back(std::move(e));
    data["mechanics"] = std::move(list);
    if (!fs::write_file_atomic(*file_path, lib::front_matter::stringify(body, data)))
        throw std::runtime_error("cannot write game file: " + *file_path);
    return count;
}

GameMechanicEntry mechanic_summary_to_entry(const MechanicSummary& m) {
    GameMechanicEntry e;
    e.name = m.name;
    e.description = m.description;
    e.intended_affect = m.intended_affect;
    e.intended_valence = m.intended_valence;
    e.intended_aspects = m.intended_aspects;
    e.intended_emotions = m.intended_emotions;
    e.source = m.source;
    return e;
}

} // namespace di::flow
