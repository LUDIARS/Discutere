#pragma once
// 未分類発話のクラスタ (src/core/projection/command-handlers/cross/cluster.ts)。
#include <string>
#include <vector>

#include "core/cache/uncategorized_clusters.hpp"
#include "core/projection/types.hpp"

namespace di::core::projection::command_handlers::cross {

/** normalized_content に [uncategorized] を含む発話を先頭語で束ね、キャッシュに入れて返す。 */
std::vector<cache::Cluster> run_uncategorized_clustering_job(ReturnTypeCreateCore& core, const std::string& workspace_id);

/** キャッシュ (無ければ集計) を "- key: count" の行にする。 */
std::string handle_cluster(ReturnTypeCreateCore& core, const std::string& workspace_id);

} // namespace di::core::projection::command_handlers::cross
