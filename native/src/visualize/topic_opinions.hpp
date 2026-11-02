#pragma once
// 話題 (design_gaps) と意見 (hypotheses) のスナップショット (src/visualize/topic-opinions.ts の移植)。
// 戻り値は TS の TopicOpinionSnapshot と同じ形の JSON。
#include <optional>
#include <string>

#include "base/json.hpp"
#include "base/sqlite.hpp"

namespace di::visualize {

struct TopicOpinionOptions {
    std::optional<double> limit;             // 既定 40 (1..200)
    std::optional<double> opinionsPerTopic;  // 既定 8 (1..50)
};

Json build_topic_opinion_snapshot(sql::Database& db, const std::string& workspace_id, const TopicOpinionOptions& options = {});

/** Number.isFinite でなければ min、そうでなければ floor して [min, max] に収める。 */
std::int64_t clamp_limit(double value, std::int64_t min, std::int64_t max);

} // namespace di::visualize
