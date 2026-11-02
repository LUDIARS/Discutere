#pragma once
// src/fundamentum-export/list.ts の移植: export 済み議論の一覧・個別取得・バージョン履歴。
// SRP: catalog / master からの読み出しのみ (export の実行は export)。
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "base/json.hpp"

namespace di::fundamentum_export {

struct ExportedDiscussionRef {
    std::string discussionId;
    std::string contentId;
};
Json to_json(const ExportedDiscussionRef& r);

/** catalog の最新版に載っている全議論 (discussionId の localeCompare 順)。 */
std::vector<ExportedDiscussionRef> list_exported_discussions();

/** 1 議論の最新スナップショット (export 未実施なら nullopt)。 */
std::optional<Json> get_exported_discussion_snapshot(const std::string& discussion_id);

struct DiscussionRevision {
    /** catalog 全体の版番号 (他議論の export でも進む点に注意)。 */
    std::int64_t version = 0;
    std::string contentId;
};
Json to_json(const DiscussionRevision& r);

/** 1 議論の content id がバージョンを跨いでどう変わったか (古い → 新しい順、変化点だけ)。 */
std::vector<DiscussionRevision> get_discussion_export_history(const std::string& discussion_id);

} // namespace di::fundamentum_export
