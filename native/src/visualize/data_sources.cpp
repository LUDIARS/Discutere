#include "visualize/data_sources.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <vector>

#include "base/strings.hpp"
#include "base/time.hpp"
#include "visualize/locale_compare.hpp"

namespace di::visualize {
namespace {

struct SourceMeta {
    std::string label;
    std::string kind;  // "import" | "internal" | "derived"
    std::string origin;
};

/** 既知 source のメタ。session.title の接頭辞 (`<source>:`) をキーにする。 */
SourceMeta meta_for(const std::string& source) {
    static const std::map<std::string, SourceMeta> kMeta{
        {"steam", {"Steam レビュー", "import", "store.steampowered.com appreviews API"}},
        {"youtube", {"YouTube コメント", "import", "YouTube Data API v3"}},
        {"niconico", {"ニコニコ動画 コメント", "import", "nicovideo snapshot + nvComment"}},
        {"website", {"Web 記事", "import", "任意 URL の HTML 本文抽出"}},
        {"fandom", {"Fandom / Wiki", "import", "MediaWiki action=parse"}},
        {"reddit", {"Reddit", "import", "Reddit OAuth API"}},
        {"opencritic", {"OpenCritic", "import", "OpenCritic (RapidAPI)"}},
        {"discussion-of-gap", {"自走議論 (facilitator/persona)", "internal", "Discatier 内部生成"}},
        {"discord-session", {"Discord 議論", "internal", "Discord guild"}},
        {"reception", {"観測 affect 配線", "derived", "wire:affects (sentiment.json)"}},
    };
    auto it = kMeta.find(source);
    if (it != kMeta.end()) return it->second;
    return {source, "internal", "不明"};
}

/** session.title から (source, slug) を切り出す。title 無しは scene で判定。 */
std::pair<std::string, std::string> split_title(const std::optional<std::string>& title, const std::optional<std::string>& scene) {
    if (title && !title->empty()) {
        const auto i = title->find(':');
        if (i != std::string::npos) return {title->substr(0, i), title->substr(i + 1)};
        return {*title, "(none)"};
    }
    if (scene && str::starts_with(*scene, "discord:")) return {"discord-session", "(discord)"};
    if (scene && str::starts_with(*scene, "gap:")) return {"discussion-of-gap", "(gap)"};
    return {"(none)", "(none)"};
}

bool is_hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

/** /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i */
bool is_uuid(std::string_view s) {
    if (s.size() != 36) return false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        if (dash ? s[i] != '-' : !is_hex(s[i])) return false;
    }
    return true;
}

/** slug が「ゲーム取込」を指すか (gap uuid / (gap) / (discord) / (none) は除外)。 */
bool is_game_slug(const std::string& slug) {
    if (slug.empty() || slug[0] == '(') return false;
    return !is_uuid(slug);
}

struct Acc {
    std::int64_t sessions = 0;
    std::int64_t utterances = 0;
    std::set<std::string> games;  // 出力時に並べ直すので順は問わない
};

} // namespace

Json build_data_source_snapshot(sql::Database& db, const std::string& workspace_id) {
    return build_data_source_snapshot(db, workspace_id, time::now_ms());
}

Json build_data_source_snapshot(sql::Database& db, const std::string& workspace_id, std::int64_t now) {
    const auto sessions = db.all("SELECT id, title, scene FROM sessions WHERE workspace_id = ?", {workspace_id});
    std::map<std::string, std::int64_t> utt_by_session;
    for (const auto& r : db.all("SELECT session_id, COUNT(*) AS c FROM utterances WHERE workspace_id = ? GROUP BY session_id",
                                {workspace_id}))
        utt_by_session[r.str("session_id")] = r.i64("c");

    std::vector<std::pair<std::string, Acc>> by_source;
    std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::pair<std::int64_t, std::int64_t>>>>> by_game;
    // Map の挿入順を保ちつつ、引くのは索引で (件数が多くても線形)。
    std::map<std::string, std::size_t> source_index;
    std::map<std::string, std::size_t> game_index;
    std::map<std::pair<std::string, std::string>, std::size_t> game_source_index;
    auto find_source = [&](const std::string& s) -> Acc& {
        auto [it, inserted] = source_index.emplace(s, by_source.size());
        if (inserted) by_source.emplace_back(s, Acc{});
        return by_source[it->second].second;
    };

    for (const auto& s : sessions) {
        const auto [source, slug] = split_title(s.opt_str("title"), s.opt_str("scene"));
        const auto it = utt_by_session.find(s.str("id"));
        const std::int64_t utt = it == utt_by_session.end() ? 0 : it->second;
        Acc& acc = find_source(source);
        acc.sessions += 1;
        acc.utterances += utt;
        if (is_game_slug(slug)) {
            acc.games.insert(slug);
            auto [gi, g_new] = game_index.emplace(slug, by_game.size());
            if (g_new) by_game.emplace_back(slug, std::vector<std::pair<std::string, std::pair<std::int64_t, std::int64_t>>>{});
            auto& srcs = by_game[gi->second].second;
            auto [si, s_new] = game_source_index.emplace(std::make_pair(slug, source), srcs.size());
            if (s_new) srcs.emplace_back(source, std::make_pair(std::int64_t{0}, std::int64_t{0}));
            srcs[si->second].second.first += 1;
            srcs[si->second].second.second += utt;
        }
    }

    struct SourceOut {
        Json json;
        std::int64_t utterances;
        bool import;
    };
    std::vector<SourceOut> sources;
    for (auto& [source, acc] : by_source) {
        const auto meta = meta_for(source);
        std::vector<std::string> games(acc.games.begin(), acc.games.end());
        std::stable_sort(games.begin(), games.end(), [](const auto& a, const auto& b) { return js_default_compare(a, b) < 0; });
        Json j = Json::object();
        j["source"] = source;
        j["label"] = meta.label;
        j["kind"] = meta.kind;
        j["origin"] = meta.origin;
        j["sessions"] = acc.sessions;
        j["utterances"] = acc.utterances;
        j["games"] = games;
        j["gameCount"] = acc.games.size();
        j["size"] = acc.utterances;
        sources.push_back({std::move(j), acc.utterances, meta.kind == "import"});
    }
    std::stable_sort(sources.begin(), sources.end(), [](const auto& a, const auto& b) { return b.utterances < a.utterances; });

    struct GameOut {
        Json json;
        std::int64_t utterances;
    };
    std::vector<GameOut> games_out;
    for (auto& [slug, src_map] : by_game) {
        struct SrcOut {
            Json json;
            std::int64_t sessions;
            std::int64_t utterances;
        };
        std::vector<SrcOut> srcs;
        for (auto& [source, v] : src_map) {
            const auto meta = meta_for(source);
            Json j = Json::object();
            j["source"] = source;
            j["label"] = meta.label;
            j["kind"] = meta.kind;
            j["sessions"] = v.first;
            j["utterances"] = v.second;
            srcs.push_back({std::move(j), v.first, v.second});
        }
        std::stable_sort(srcs.begin(), srcs.end(), [](const auto& a, const auto& b) { return b.utterances < a.utterances; });
        std::int64_t sess = 0, utt = 0;
        Json arr = Json::array();
        for (auto& s : srcs) {
            sess += s.sessions;
            utt += s.utterances;
            arr.push_back(std::move(s.json));
        }
        Json j = Json::object();
        j["slug"] = slug;
        j["sessions"] = sess;
        j["utterances"] = utt;
        j["sources"] = arr;
        games_out.push_back({std::move(j), utt});
    }
    std::stable_sort(games_out.begin(), games_out.end(), [](const auto& a, const auto& b) { return b.utterances < a.utterances; });

    std::int64_t import_sources = 0, total_utt = 0, import_utt = 0;
    Json sources_json = Json::array();
    for (auto& s : sources) {
        total_utt += s.utterances;
        if (s.import) {
            ++import_sources;
            import_utt += s.utterances;
        }
        sources_json.push_back(std::move(s.json));
    }
    Json by_game_json = Json::array();
    for (auto& g : games_out) by_game_json.push_back(std::move(g.json));

    Json totals = Json::object();
    totals["sources"] = sources.size();
    totals["importSources"] = import_sources;
    totals["internalSources"] = static_cast<std::int64_t>(sources.size()) - import_sources;
    totals["utterances"] = total_utt;
    totals["importUtterances"] = import_utt;
    totals["games"] = by_game.size();
    Json out = Json::object();
    out["workspaceId"] = workspace_id;
    out["generatedAt"] = now;
    out["totals"] = totals;
    out["sources"] = sources_json;
    out["byGame"] = by_game_json;
    return out;
}

} // namespace di::visualize
