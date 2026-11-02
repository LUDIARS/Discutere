#pragma once
// 共有 KG の取り込み (follower 側, src/kg-sync/import.ts)。
// NDJSON を解析し、共有テーブルへ id で upsert する。競合は updated_at の last-write-wins
// (受信行の updated_at が既存以上のときだけ上書き)。whitelist 外のテーブル名は無視 (任意行注入を防止)。
// master/follower で列差があっても双方に存在する列だけを書き込む。全体を 1 つの transaction で行い、
// 途中で例外が出たら何も書かない (TS と同じ。null の行や object の値は better-sqlite3 と同じく例外)。
#include <cstddef>
#include <string_view>

#include "base/sqlite.hpp"

namespace di::kg_sync {

struct KgImportResult {
    /** 挿入 or 上書きした行数。 */
    std::size_t applied = 0;
    /** LWW で古いと判定し skip した行数。 */
    std::size_t skipped = 0;
    /** whitelist 外テーブル・壊れた行・上限を超えた行で無視した行数。 */
    std::size_t ignored = 0;
    /** 取り込んだ行の最大 updated_at。 */
    double watermark = 0;
};

/** 外部入力 (pull した HTTP 応答・受け取った本文) の上限。 */
struct KgImportLimits {
    /** NDJSON 全体の大きさ。超えたら何も書かずに std::length_error。 */
    std::size_t max_bytes = 256u << 20;
    /** 行数。超えたら何も書かずに std::length_error。 */
    std::size_t max_lines = 2'000'000;
    /** 1 行の大きさ・要素数・深さ。超えた行は壊れた行と同じく ignored。 */
    std::size_t max_line_bytes = 4u << 20;
    std::size_t max_line_elements = 100'000;
    std::size_t max_line_depth = 32;
};

KgImportResult import_shared_kg(sql::Database& db, std::string_view ndjson, const KgImportLimits& limits = {});

} // namespace di::kg_sync
