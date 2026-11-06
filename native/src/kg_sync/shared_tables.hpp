#pragma once
// KG 共有同期で扱う「共有資産」テーブルの定義 (src/kg-sync/shared-tables.ts, spec/data/core/KG-SYNC.md)。
// master が shared=1 を立てた行だけが follower へ配布される (学習知識 4 テーブル)。
// 列はハードコードせず実行時に PRAGMA table_info で取得する。
#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "base/sqlite.hpp"

namespace di::kg_sync {

/** NDJSON フォーマットのスキーマ版。 */
inline constexpr int KG_SYNC_SCHEMA_VERSION = 1;

/** 同期対象テーブル (whitelist)。import 側はここに無いテーブルを拒否する (任意行注入の防止)。 */
inline constexpr std::array<std::string_view, 4> SHARED_TABLES = {"games", "mechanics", "aesthetics", "affects"};

/** 透かし列 (増分の基準)。 */
inline constexpr std::string_view WATERMARK_COLUMN = "updated_at";
/** 共有フラグ列。 */
inline constexpr std::string_view SHARED_FLAG_COLUMN = "shared";

bool is_shared_table(std::string_view name);

/**
 * テーブルの実列名を PRAGMA で取得する。存在しなければ std::runtime_error。
 * (TS は DB ごとにキャッシュしていた。C++ は export/import の 1 回の呼び出しの中でだけ使い回す。)
 */
std::vector<std::string> table_columns(sql::Database& db, std::string_view table);

/** SQL の識別子を "..." で囲む ("" は二重にする)。 */
std::string quote_identifier(std::string_view name);

} // namespace di::kg_sync
