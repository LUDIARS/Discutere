#pragma once
// 発話ノイズ除外 (per-utterance soft-exclude, src/core/noise/exclusions.ts)。
// utterances テーブルには触らず、サイドカー表 utterance_exclusions に除外 id を積む。
// 除外は可逆 (restore で DELETE)。読み取り側は EXCLUDED_NOT_IN_SQL で弾く。
#include <optional>
#include <string>
#include <unordered_set>

#include "base/sqlite.hpp"

namespace di::core::noise {

/** WHERE 句で再利用する除外フィルタの定数 SQL フラグメント。 */
inline constexpr const char* EXCLUDED_NOT_IN_SQL = "id NOT IN (SELECT utterance_id FROM utterance_exclusions)";

/** サイドカー表を冪等に確保する。 */
void ensure_exclusion_table(sql::Database& raw);
/** 発話をノイズとして除外する (INSERT OR REPLACE で冪等)。 */
void exclude_utterance(sql::Database& raw, const std::string& utterance_id, const std::optional<std::string>& reason = std::nullopt);
/** 除外を取り消す (元に戻す)。 */
void restore_utterance(sql::Database& raw, const std::string& utterance_id);
/** 指定発話が除外済みか。 */
bool is_excluded(sql::Database& raw, const std::string& utterance_id);
/** 除外済み id の集合。 */
std::unordered_set<std::string> list_excluded_ids(sql::Database& raw);

} // namespace di::core::noise
