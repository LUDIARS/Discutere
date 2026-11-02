#pragma once
// src/visualize/conclusion-cache.ts の移植: 結論一覧 (まとめ) の読み取りキャッシュ (conclusion_cache, discutere.db)。
//   - 議論が収束するたびに upsert_conclusion_cache で 1 行 write-through 更新する (KG 非依存)。
//   - 一覧閲覧 (/learning/conclusions) は list_cached_conclusions でこのテーブルだけ読む。
//   - 話題に紐づく外部クロール材料件数 (materialCount) は KG を引く重い計算なので
//     recompute_material_counts で別途 (build:conclusion-cache) まとめて算出・更新する。
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "base/sqlite.hpp"
#include "visualize/conclusions.hpp"

namespace di::visualize {

/** materialCount 未算出マーカー (KG 別途計算で埋める)。 */
inline constexpr std::int64_t MATERIAL_COUNT_UNCOMPUTED = -1;
/** Number.MAX_SAFE_INTEGER (「全件」の limit)。 */
inline constexpr std::int64_t kMaxSafeInteger = 9007199254740991LL;

struct UpsertOptions {
    /** 議論ボリューム。未指定なら summary.utteranceCount を使う。 */
    std::optional<std::int64_t> discussionVolume;
    /** 外部クロール材料件数。未指定/未算出は温存 (既存値を壊さない)。 */
    std::optional<std::int64_t> materialCount;
    /** テスト用に注入する現在時刻。 */
    std::optional<std::int64_t> now;
};

/** 1 結論をキャッシュへ upsert する (write-through)。 */
void upsert_conclusion_cache(sql::Database& db, const ConclusionSummary& summary, const UpsertOptions& opts = {});

struct WriteThroughParams {
    std::string sessionId;
    std::string title;
    /** 収束まとめ本文 (【収束】プレフィックス除去済 / 未収束は nullopt)。 */
    std::optional<std::string> conclusion;
    std::int64_t utteranceCount = 0;
    std::int64_t aufhebungCount = 0;
    std::optional<std::int64_t> updatedAt;
};

/** 収束時 (flow/conclusion) の write-through 専用。KG を一切引かず手元の発話数/止揚数だけで更新する。 */
void write_through_flow_conclusion(sql::Database& db, const WriteThroughParams& params);

/** キャッシュ済み結論を新しい順に一覧する (KG 非タッチ)。 */
std::vector<ConclusionSummary> list_cached_conclusions(sql::Database& db, std::int64_t limit = 100);

/** キャッシュ行数 (fallback 判定用)。 */
std::int64_t conclusion_cache_count(sql::Database& db);

/**
 * 新フロー結論をキャッシュへ取り込んで「追いついた」状態を保証する (自己修復・KG 非依存)。
 * キャッシュ件数が flow_conclusion 総数に満たない時だけ全 flow 結論を upsert し直す。
 */
void ensure_conclusion_cache_fresh(sql::Database& db);

/** 1 行の materialCount を更新する (KG 別途計算の書き戻し)。 */
void set_material_count(sql::Database& db, const std::string& gap_id, std::int64_t count);

/**
 * 全キャッシュ行について materialCount を count_material で算出して書き戻す。
 * count_material が投げた行は飛ばす (1 件の失敗で全体を止めない)。非有限・負の値は書かない。
 * @returns 対象にした行数。
 */
std::int64_t recompute_material_counts(sql::Database& db, const std::function<double(const ConclusionSummary&)>& count_material,
                                       bool only_missing = false);

} // namespace di::visualize
