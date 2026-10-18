import { createHash } from "node:crypto";
import type Database from "better-sqlite3";

export type PublicationScope = "private" | "public" | "glab";
export interface PublishedConclusion {
  id: string;
  title: string;
  conclusion: string;
  updatedAt: number;
  scope: "public" | "glab";
}
interface ConclusionRow { gap_id: string; title: string; conclusion: string | null; updated_at: number }
interface PublicationRow { publishable: number; scope: PublicationScope; content_hash: string }

function contentHash(row: ConclusionRow): string {
  return createHash("sha256").update(JSON.stringify([row.title, row.conclusion, row.updated_at])).digest("hex");
}

export function publicationState(db: Database.Database, id: string): { publishable: boolean; scope: PublicationScope; revision: string; title: string; conclusion: string } | null {
  const row = db.prepare("SELECT gap_id, title, conclusion, updated_at FROM conclusion_cache WHERE gap_id = ?").get(id) as ConclusionRow | undefined;
  if (!row) return null;
  const setting = db.prepare("SELECT publishable, scope, content_hash FROM conclusion_publication WHERE conclusion_id = ?").get(id) as PublicationRow | undefined;
  return { publishable: !!setting?.publishable && setting.content_hash === contentHash(row), scope: setting?.scope ?? "private", revision: contentHash(row), title: row.title, conclusion: row.conclusion ?? "" };
}

export function setPublication(db: Database.Database, id: string, publishable: boolean, scope: PublicationScope, revision: string, now: number): boolean {
  return db.transaction(() => {
    const row = db.prepare("SELECT gap_id, title, conclusion, updated_at FROM conclusion_cache WHERE gap_id = ?").get(id) as ConclusionRow | undefined;
    if (!row || contentHash(row) !== revision || (publishable && !row.conclusion?.trim())) return false;
    db.prepare(`INSERT INTO conclusion_publication VALUES (?, ?, ?, ?, ?)
      ON CONFLICT(conclusion_id) DO UPDATE SET publishable=excluded.publishable, scope=excluded.scope,
        content_hash=excluded.content_hash, updated_at=excluded.updated_at`).run(id, Number(publishable), scope, contentHash(row), now);
    return true;
  })();
}

/** No raw transcript, author identifiers or paper supplements cross this boundary. */
export function publishedConclusions(db: Database.Database, id?: string): PublishedConclusion[] {
  const rows = db.prepare(`SELECT c.gap_id, c.title, c.conclusion, c.updated_at, p.scope, p.content_hash
    FROM conclusion_cache c JOIN conclusion_publication p ON p.conclusion_id=c.gap_id
    WHERE p.publishable=1 AND p.scope IN ('public','glab') ${id === undefined ? "" : "AND c.gap_id=?"}
    ORDER BY c.updated_at DESC, c.gap_id`).all(...(id === undefined ? [] : [id])) as (ConclusionRow & PublicationRow)[];
  return rows.filter(row => row.conclusion?.trim() && row.content_hash === contentHash(row))
    .slice(0, 200).map(row => ({ id: row.gap_id, title: row.title, conclusion: row.conclusion ?? "", updatedAt: row.updated_at, scope: row.scope as "public" | "glab" }));
}
