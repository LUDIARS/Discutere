#pragma once
// 議論キューのスナップショット生成 (議論の展開 / 現状積まれているキューの可視化)。
// 移植元 src/queue/snapshot.ts。
//   - activeSessions: discord-bound でまだ閉じていない議論 session (発話数 / 最終発話 / 発火数)
//   - openGaps:       未解決の DesignGap
//   - pendingHypotheses: 未統合・未棄却の Hypothesis
// Discatier Core (sqlite) を読み取り専用で参照し、発火数のみ persona-engine db を見る。
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "base/sqlite.hpp"

namespace di::queue {

struct QueueSessionEntry {
    std::string sessionId;
    std::string title;
    std::optional<std::string> scene;
    std::optional<std::string> guildId;
    std::optional<std::string> channelId;
    Json startedAt;        // DB の値 (JS の number)
    std::int64_t utteranceCount = 0;
    Json lastUtteranceAt;  // number | null
    Json fireCount;        // number
};

struct QueueGapEntry {
    std::string id;
    std::string title;
    std::optional<std::string> status;
};

struct QueueHypothesisEntry {
    std::string id;
    std::string statement;
    std::optional<std::string> status;
    std::optional<std::string> designGapId;
};

struct QueueSnapshot {
    std::string workspaceId;
    std::int64_t generatedAt = 0;
    std::vector<QueueSessionEntry> activeSessions;
    std::vector<QueueGapEntry> openGaps;
    std::vector<QueueHypothesisEntry> pendingHypotheses;
};

Json to_json(const QueueSnapshot& snap);

/**
 * @param core_db Discatier Core の sqlite
 * @param pe_db   persona-engine の sqlite (session_rule_fires 用)。nullptr なら fireCount=0
 * @param now_ms  generatedAt (既定 Date.now())
 */
QueueSnapshot build_queue_snapshot(sql::Database& core_db, sql::Database* pe_db, const std::string& workspace_id,
                                   std::optional<std::int64_t> now_ms = std::nullopt);

/** slash / ログ向けの短い人間可読サマリ (Discord 2000 字制限を意識して上位のみ、1900 字で切る)。 */
std::string format_queue_text(const QueueSnapshot& snap);

} // namespace di::queue
