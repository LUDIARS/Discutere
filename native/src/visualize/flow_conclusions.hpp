#pragma once
// src/visualize/flow-conclusions.ts の移植: 新フロー (src/flow/) の議論結論を学習ビューの「結論」タブに載せる。
// flow_* テーブル (discussion_paper / flow_utterance / flow_conclusion / vote) を読み、conclusions と同じ
// ConclusionSummary / ConclusionDetail 形状で返す。一意キーは "flow:<sessionId>"。
// flow_utterance は内部ペルソナのみなので出所は null。
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/sqlite.hpp"
#include "visualize/conclusions.hpp"

namespace di::visualize {

/** "flow:<sessionId>" 形式の合成 id の接頭辞。 */
inline constexpr std::string_view FLOW_CONCLUSION_PREFIX = "flow:";

bool is_flow_conclusion_id(std::string_view gap_id);
std::string flow_session_id_from_gap_id(std::string_view gap_id);

/** DB の JSON 列 (aufhebung_json / tags_json / mechanics_json / top_utterance_ids_json) の上限。 */
inline constexpr std::size_t kFlowConclusionJsonMaxBytes = 4u << 20;

/** 新フロー結論を新しい順に一覧する (flow_conclusion 全件、limit 件まで)。 */
std::vector<ConclusionSummary> list_flow_conclusions(sql::Database& db, std::int64_t limit = 100);

/** 1 件の新フロー結論の裏にある論述データ (議論ログ / 止揚 / 高評価意見)。無ければ nullopt。 */
std::optional<ConclusionDetail> get_flow_conclusion_detail(sql::Database& db, const std::string& session_id);

} // namespace di::visualize
