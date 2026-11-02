#pragma once
// mechanic のホットスポット順位 (src/core/projection/command-handlers/cross/hotspot.ts)。
#include <string>
#include <vector>

#include "core/cache/hotspot_rank.hpp"
#include "core/projection/types.hpp"

namespace di::core::projection::command_handlers::cross {

/** mechanic ごとに 発話数*0.5 + gap 数*2 + stale 仮説数*1.5 を出して降順に並べ、キャッシュに入れて返す。 */
std::vector<cache::Rank> run_hotspot_aggregation_job(ReturnTypeCreateCore& core, const std::string& workspace_id);

/** キャッシュ (無ければ集計) の上位 10 件を整形する。 */
std::string handle_hotspot(ReturnTypeCreateCore& core, const std::string& workspace_id);

} // namespace di::core::projection::command_handlers::cross
