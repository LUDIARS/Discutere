#pragma once
// 共有 KG の差分エクスポート (master 側, src/kg-sync/export.ts)。
// 各共有テーブルの `shared=1 AND updated_at > since` の行を NDJSON で返す。
// 1 行目は manifest (watermark = 含めた行の最大 updated_at)。
#include <cstddef>
#include <optional>
#include <string>

#include "base/sqlite.hpp"

namespace di::kg_sync {

struct KgExportOptions {
    /** この時刻 (epoch ms) より後に更新された行のみ。既定 0 = 全件。 */
    std::optional<double> since;
    /** 指定時 (空でないとき) はこの workspace の行のみ。 */
    std::optional<std::string> workspaceId;
    /** 1 テーブルから読む行・バイトの上限 (DB の値も外部入力として課金する)。 */
    std::size_t max_rows_per_table = 1'000'000;
    std::size_t max_bytes_per_table = 512u << 20;
};

struct KgExportResult {
    /** NDJSON 本文 (末尾改行付き)。 */
    std::string ndjson;
    /** 含めた行の最大 updated_at (0 件なら since と同値)。 */
    double watermark = 0;
    std::size_t count = 0;
};

KgExportResult export_shared_kg(sql::Database& db, const KgExportOptions& opts = {});

} // namespace di::kg_sync
