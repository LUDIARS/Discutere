#pragma once
// src/fundamentum-export/export.ts の移植: 議論を Fundamentum master に put し、
// catalog "discutere/discussions" へ論理名 (gapId) で登録する。SRP: export の実行 (I/O) のみ。
#include <cstdint>
#include <string>

#include "base/json.hpp"
#include "visualize/conclusions.hpp"

namespace di::fundamentum_export {

inline constexpr const char* DISCUSSIONS_CATALOG = "discutere/discussions";

struct FundamentumExportResult {
    std::string discussionId;
    std::string contentId;
    std::int64_t catalogVersion = 0;
};
Json to_json(const FundamentumExportResult& r);

FundamentumExportResult export_discussion_to_fundamentum(const visualize::ConclusionDetail& detail);

} // namespace di::fundamentum_export
