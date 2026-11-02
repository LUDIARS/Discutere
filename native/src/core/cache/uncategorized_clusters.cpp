#include "core/cache/uncategorized_clusters.hpp"

#include <cstdint>
#include <map>
#include <mutex>

#include "base/time.hpp"

namespace di::core::cache {
namespace {
struct Entry {
    std::int64_t at;
    std::vector<Cluster> value;
};
std::mutex g_mutex;
std::map<std::string, Entry>& entries() {
    static std::map<std::string, Entry> m;
    return m;
}
} // namespace

Json to_json(const Cluster& c) { return Json{{"key", c.key}, {"count", c.count}, {"sample", c.sample}}; }

void set_uncategorized_clusters_cache(const std::string& workspace_id, std::vector<Cluster> value) {
    std::lock_guard lock(g_mutex);
    entries()[workspace_id] = Entry{time::now_ms(), std::move(value)};
}

std::optional<std::vector<Cluster>> get_uncategorized_clusters_cache(const std::string& workspace_id) {
    std::lock_guard lock(g_mutex);
    auto it = entries().find(workspace_id);
    if (it == entries().end()) return std::nullopt;
    return it->second.value;
}

} // namespace di::core::cache
