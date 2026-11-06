#pragma once
// データソース・スナップショット (read-only。src/visualize/data-sources.ts の移植)。
// KG (Core = SQLite) の sessions / utterances を走査し、取得元 (source) ごとの件数を集計する。
// session.title は `<source>:<slug>` (ext 取込) / `discussion-of-gap:<gapId>` (自走議論) 等の規約なので、
// 接頭辞を source として切り出す。戻り値は TS の DataSourceSnapshot と同じ形の JSON。
#include <cstdint>
#include <string>

#include "base/json.hpp"
#include "base/sqlite.hpp"

namespace di::visualize {

Json build_data_source_snapshot(sql::Database& db, const std::string& workspace_id, std::int64_t now);
Json build_data_source_snapshot(sql::Database& db, const std::string& workspace_id);

} // namespace di::visualize
