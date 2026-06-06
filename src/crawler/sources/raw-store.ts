/**
 * raw-store — 長文 source の raw 全文を退避する sidecar (id=67)。
 *
 * 議論コンテキスト参照はトークン節約のため要約 (utterances.raw_content = summary) を
 * 使い、 raw 全文はここに per-utterance で保管して必要時のみ引く 2 層化。
 * utterances 本体の schema は不変 (ingested-store と同じ思想)。
 */

import fs from "node:fs";
import path from "node:path";

import Database from "better-sqlite3";

import type { ExternalSource } from "./types.js";

export interface RawDocument {
  utteranceId: string;
  source: string;
  url: string;
  rawText: string;
  savedAt: number;
}

export interface RawStore {
  /** utteranceId に raw 全文を紐付けて保存 (上書き)。 */
  set(input: { utteranceId: string; source: ExternalSource | string; url: string; rawText: string; at?: number }): void;
  /** utteranceId の raw 全文を引く。 無ければ null。 */
  get(utteranceId: string): RawDocument | null;
  close(): void;
}

export const DEFAULT_RAW_PATH = path.resolve("./data/external/.raw.sqlite");

export function openRawStore(dbPath: string = DEFAULT_RAW_PATH): RawStore {
  fs.mkdirSync(path.dirname(dbPath), { recursive: true });
  const db = new Database(dbPath);
  db.pragma("journal_mode = WAL");
  db.exec(
    `CREATE TABLE IF NOT EXISTS source_raw (
       utterance_id TEXT PRIMARY KEY,
       source TEXT NOT NULL,
       url TEXT NOT NULL,
       raw_text TEXT NOT NULL,
       saved_at INTEGER NOT NULL
     )`
  );
  const setStmt = db.prepare(
    `INSERT INTO source_raw (utterance_id, source, url, raw_text, saved_at)
       VALUES (@utteranceId, @source, @url, @rawText, @savedAt)
     ON CONFLICT(utterance_id) DO UPDATE SET
       source = excluded.source, url = excluded.url, raw_text = excluded.raw_text, saved_at = excluded.saved_at`
  );
  const getStmt = db.prepare("SELECT utterance_id, source, url, raw_text, saved_at FROM source_raw WHERE utterance_id = ?");
  return {
    set: (input) => {
      setStmt.run({
        utteranceId: input.utteranceId,
        source: String(input.source),
        url: input.url,
        rawText: input.rawText,
        savedAt: input.at ?? Date.now(),
      });
    },
    get: (utteranceId) => {
      const row = getStmt.get(utteranceId) as
        | { utterance_id: string; source: string; url: string; raw_text: string; saved_at: number }
        | undefined;
      if (!row) return null;
      return { utteranceId: row.utterance_id, source: row.source, url: row.url, rawText: row.raw_text, savedAt: row.saved_at };
    },
    close: () => db.close(),
  };
}
