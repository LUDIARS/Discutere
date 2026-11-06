#include "kg_sync/state.hpp"

#include <stdexcept>

#include "base/fsutil.hpp"

namespace di::kg_sync {

Json to_json(const SourceSyncState& s) {
    return Json{{"watermark", s.watermark}, {"lastPulledAt", s.lastPulledAt}, {"lastApplied", s.lastApplied}};
}

KgSyncState read_sync_state(const std::string& state_path) {
    const std::string abs = fs::resolve(state_path);
    KgSyncState state;
    if (!fs::exists(abs)) return state;
    auto text = fs::read_file(abs, kMaxSyncStateBytes);
    if (!text) return state;
    auto parsed = parse_json(*text, {kMaxSyncStateBytes, 1'000'000, 16});
    if (!parsed || !parsed->is_object()) return state;
    auto it = parsed->find("sources");
    if (it != parsed->end() && it->is_object()) state.sources = *it;
    return state;
}

void write_sync_state(const KgSyncState& state, const std::string& state_path) {
    const std::string abs = fs::resolve(state_path);
    fs::ensure_dir(fs::dirname(abs));
    const Json doc = {{"sources", state.sources.is_object() ? state.sources : Json::object()}};
    if (!fs::write_file_atomic(abs, json_stringify_pretty(doc, 2)))
        throw std::runtime_error("kg-sync: 同期状態ファイルを書けない");
}

double get_source_watermark(const KgSyncState& state, const std::string& url) {
    if (!state.sources.is_object()) return 0;
    auto it = state.sources.find(url);
    if (it == state.sources.end() || !it->is_object()) return 0;
    auto w = it->find("watermark");
    return w != it->end() && w->is_number() ? w->get<double>() : 0;
}

void set_source_state(KgSyncState& state, const std::string& url, const SourceSyncState& next) {
    if (!state.sources.is_object()) state.sources = Json::object();
    state.sources[url] = to_json(next);
}

} // namespace di::kg_sync
