#include "core/cache/hotspot_rank.hpp"

#include <cstdint>
#include <map>
#include <mutex>

#include "base/time.hpp"

namespace di::core::cache {
namespace {
struct Entry {
    std::int64_t at;
    std::vector<Rank> value;
};
std::mutex g_mutex;
std::map<std::string, Entry>& entries() {
    static std::map<std::string, Entry> m;
    return m;
}
} // namespace

Json to_json(const Rank& r) {
    return Json{{"mechanicId", r.mechanicId}, {"name", r.name},         {"utteranceCount", r.utteranceCount},
                {"gapCount", r.gapCount},     {"staleCount", r.staleCount}, {"score", r.score}};
}

void set_hotspot_rank_cache(const std::string& workspace_id, std::vector<Rank> value) {
    std::lock_guard lock(g_mutex);
    entries()[workspace_id] = Entry{time::now_ms(), std::move(value)};
}

std::optional<std::vector<Rank>> get_hotspot_rank_cache(const std::string& workspace_id) {
    std::lock_guard lock(g_mutex);
    auto it = entries().find(workspace_id);
    if (it == entries().end()) return std::nullopt;
    return it->second.value;
}

} // namespace di::core::cache
