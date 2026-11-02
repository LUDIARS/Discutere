#pragma once
// ホットスポット順位のメモリキャッシュ (src/core/cache/hotspot-rank.ts)。workspace ごとに最後の値を持つ。
#include <optional>
#include <string>
#include <vector>

#include "base/json.hpp"

namespace di::core::cache {

struct Rank {
    std::string mechanicId;
    std::string name;
    double utteranceCount = 0;
    double gapCount = 0;
    double staleCount = 0;
    double score = 0;
};

Json to_json(const Rank& r);

void set_hotspot_rank_cache(const std::string& workspace_id, std::vector<Rank> value);
/** 無ければ nullopt (TS の null)。 */
std::optional<std::vector<Rank>> get_hotspot_rank_cache(const std::string& workspace_id);

} // namespace di::core::cache
