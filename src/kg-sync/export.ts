/**
 * 共有 KG の差分エクスポート (master 側) — spec/core/KG-SYNC.md。
 *
 * 各共有テーブルの `shared=1 AND updated_at > since` の行を NDJSON で返す。
 * 1 行目は manifest (watermark = 含めた行の最大 updated_at)。 follower はこの watermark を
 * 保存し、次回 since に渡すことで増分のみ取得する。
 */

import type Database from "better-sqlite3";

import {
  KG_SYNC_SCHEMA_VERSION,
  SHARED_TABLES,
  WATERMARK_COLUMN,
  SHARED_FLAG_COLUMN,
  tableColumns,
} from "./shared-tables.js";

export interface KgExportOptions {
  /** この時刻 (epoch ms) より後に更新された行のみ。既定 0 = 全件。 */
  since?: number;
  /** 指定時はこの workspace の行のみ。 */
  workspaceId?: string;
}

export interface KgExportResult {
  /** NDJSON 本文 (末尾改行付き)。 */
  ndjson: string;
  /** 含めた行の最大 updated_at (0 件なら since と同値)。 */
  watermark: number;
  /** 行数。 */
  count: number;
}

export function exportSharedKg(db: Database.Database, opts: KgExportOptions = {}): KgExportResult {
  const since = opts.since ?? 0;
  let watermark = since;
  const rowLines: string[] = [];

  for (const table of SHARED_TABLES) {
    const cols = tableColumns(db, table);
    const where = [`${SHARED_FLAG_COLUMN} = 1`, `${WATERMARK_COLUMN} > @since`];
    const params: Record<string, unknown> = { since };
    if (opts.workspaceId) {
      where.push("workspace_id = @ws");
      params.ws = opts.workspaceId;
    }
    const selectCols = cols.map((c) => `"${c}"`).join(", ");
    const rows = db
      .prepare(`SELECT ${selectCols} FROM ${table} WHERE ${where.join(" AND ")} ORDER BY ${WATERMARK_COLUMN} ASC`)
      .all(params) as Array<Record<string, unknown>>;
    for (const data of rows) {
      const u = Number(data[WATERMARK_COLUMN]) || 0;
      if (u > watermark) watermark = u;
      rowLines.push(JSON.stringify({ type: "row", table, data }));
    }
  }

  const manifest = {
    type: "manifest",
    schemaVersion: KG_SYNC_SCHEMA_VERSION,
    since,
    watermark,
    count: rowLines.length,
    tables: SHARED_TABLES,
  };
  const ndjson = [JSON.stringify(manifest), ...rowLines].join("\n") + "\n";
  return { ndjson, watermark, count: rowLines.length };
}
