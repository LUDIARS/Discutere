#pragma once
// src/flow/paper-revisions.ts の移植: ディスカッションペーパー本文 (markdown) の版履歴ストア。
// 編集のたびに本文 md を 1 リビジョンとして追記する。「戻す」は履歴を消さず、1 手前の本文を
// 新しいリビジョンとして積み直す (前進専用 = 監査ログを失わない)。session 単位で discutere.db に置く。
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "base/json.hpp"

namespace di::flow {

/** "initial" | "llm-edit" | "crawl" | "manual" | "revert" */
using RevisionOrigin = std::string;

struct PaperRevision {
    std::int64_t rev = 0;
    std::string bodyMd;
    std::string changeSummary;
    RevisionOrigin origin;
    std::int64_t createdAt = 0;
};
Json to_json(const PaperRevision& r);

struct AppendRevisionArgs {
    std::string sessionId;
    std::string bodyMd;
    std::string changeSummary;
    RevisionOrigin origin;
};

/** 本文 md を 1 リビジョン追記し rev を返す。直前と同一本文なら追記せず既存 rev を返す。 */
std::int64_t append_revision(const AppendRevisionArgs& args);
std::optional<PaperRevision> latest_revision(const std::string& session_id);
/** 全リビジョンを古い順で返す。 */
std::vector<PaperRevision> list_revisions(const std::string& session_id);
/** 1 手前に戻せるか (リビジョンが 2 つ以上あるか)。 */
bool can_revert(const std::string& session_id);
/** 1 手前の本文を新リビジョンとして積み直す。戻せなければ nullopt。 */
std::optional<PaperRevision> revert_last(const std::string& session_id);

} // namespace di::flow
