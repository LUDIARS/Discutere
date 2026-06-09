/**
 * 共有 KG の取り込み (follower 側) — spec/core/KG-SYNC.md。
 *
 * NDJSON を解析し、共有テーブルへ id で upsert する。競合は updated_at の
 * last-write-wins (受信行の updated_at が既存以上のときだけ上書き)。
 * whitelist 外のテーブル名は無視 (任意行注入を防止)。master/follower で列差があっても
 * 双方に存在する列だけを書き込む。
 */

import type Database from "better-sqlite3";

import { WATERMARK_COLUMN, isSharedTable, tableColumns } from "./shared-tables.js";

export interface KgImportResult {
  /** 挿入 or 上書きした行数。 */
  applied: number;
  /** LWW で古いと判定し skip した行数。 */
  skipped: number;
  /** whitelist 外テーブル等で無視した行数。 */
  ignored: number;
  /** 取り込んだ行の最大 updated_at。 */
  watermark: number;
}

interface RowLine {
  type?: string;
  table?: string;
  data?: Record<string, unknown>;
}

export function importSharedKg(db: Database.Database, ndjson: string): KgImportResult {
  const result: KgImportResult = { applied: 0, skipped: 0, ignored: 0, watermark: 0 };
  const lines = ndjson.split(/\r?\n/);

  const apply = db.transaction(() => {
    for (const line of lines) {
      const trimmed = line.trim();
      if (trimmed.length === 0) continue;
      let parsed: RowLine;
      try {
        parsed = JSON.parse(trimmed) as RowLine;
      } catch {
        result.ignored++;
        continue;
      }
      if (parsed.type === "manifest") continue;
      const table = parsed.table;
      const data = parsed.data;
      if (!table || !data || typeof data !== "object" || !isSharedTable(table)) {
        result.ignored++;
        continue;
      }
      if (typeof data.id !== "string" || data.id.length === 0) {
        result.ignored++;
        continue;
      }
      const localCols = tableColumns(db, table);
      // master/follower 双方に存在する列だけを書く。id は必須。
      const cols = localCols.filter((c) => c in data);
      if (!cols.includes("id")) {
        result.ignored++;
        continue;
      }
      const placeholders = cols.map((c) => `@${c}`).join(", ");
      const colList = cols.map((c) => `"${c}"`).join(", ");
      const updates = cols.filter((c) => c !== "id").map((c) => `"${c}" = excluded."${c}"`).join(", ");
      // LWW: 受信行の updated_at が既存以上のときだけ上書き (古い更新で巻き戻さない)。
      const sql = `INSERT INTO ${table} (${colList}) VALUES (${placeholders})
        ON CONFLICT(id) DO UPDATE SET ${updates}
        WHERE excluded."${WATERMARK_COLUMN}" >= ${table}."${WATERMARK_COLUMN}"`;
      const params: Record<string, unknown> = {};
      for (const c of cols) {
        const v = data[c];
        // better-sqlite3 は boolean/object を直接 bind できないので正規化。
        params[c] = typeof v === "boolean" ? (v ? 1 : 0) : v === undefined ? null : v;
      }
      const info = db.prepare(sql).run(params);
      if (info.changes > 0) {
        result.applied++;
        const u = Number(data[WATERMARK_COLUMN]) || 0;
        if (u > result.watermark) result.watermark = u;
      } else {
        result.skipped++;
      }
    }
  });
  apply();
  return result;
}
