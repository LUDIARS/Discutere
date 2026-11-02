#pragma once
// 未分類クラスタのメモリキャッシュ (src/core/cache/uncategorized-clusters.ts)。
#include <optional>
#include <string>
#include <vector>

#include "base/json.hpp"

namespace di::core::cache {

struct Cluster {
    std::string key;
    double count = 0;
    std::vector<std::string> sample;
};

Json to_json(const Cluster& c);

void set_uncategorized_clusters_cache(const std::string& workspace_id, std::vector<Cluster> value);
std::optional<std::vector<Cluster>> get_uncategorized_clusters_cache(const std::string& workspace_id);

} // namespace di::core::cache
