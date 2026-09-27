// @implements DI-EXTERNAL-DISCUSSION
import type Database from "better-sqlite3";
import { randomUUID } from "node:crypto";
import type { ParticipationResult } from "./contracts.js";

interface Row { id: string; state: string; result: string | null; lease_until: number }
export class ParticipationStore {
  constructor(private readonly db: Database.Database) {
    db.exec(`CREATE TABLE IF NOT EXISTS external_discussion_attempts (
      id TEXT PRIMARY KEY, scene TEXT NOT NULL, state TEXT NOT NULL,
      result TEXT, human_id TEXT NOT NULL, owner TEXT NOT NULL, lease_until INTEGER NOT NULL, created_at INTEGER NOT NULL);
      CREATE INDEX IF NOT EXISTS external_discussion_scene ON external_discussion_attempts(scene,created_at);`);
  }
  claim(scene: string, id: string, humanId: string, now: number): ParticipationResult | { token: string } {
    return this.db.transaction((): ParticipationResult | { token: string } => {
      const row = this.db.prepare("SELECT * FROM external_discussion_attempts WHERE id=?").get(id) as Row | undefined;
      if (row?.state === "done") return JSON.parse(row.result ?? '{}') as ParticipationResult;
      const answered = this.db.prepare("SELECT id FROM external_discussion_attempts WHERE scene=? AND human_id=? AND state='done' AND result LIKE '%\"status\":\"proposal\"%'").get(scene, humanId);
      if (answered) return { status: "skipped" };
      if (row && row.lease_until > now) return { status: "busy" };
      const busy = this.db.prepare("SELECT id FROM external_discussion_attempts WHERE scene=? AND state='running' AND lease_until>?").get(scene, now);
      if (busy) return { status: "busy" };
      const recent = this.db.prepare("SELECT id FROM external_discussion_attempts WHERE scene=? AND state='done' AND result LIKE '%\"status\":\"proposal\"%' AND created_at>?").get(scene, now - 180_000);
      if (recent) return { status: "waiting" };
      const token = randomUUID();
      this.db.prepare(`INSERT INTO external_discussion_attempts(id,scene,state,result,human_id,owner,lease_until,created_at)
        VALUES(?,?,'running',NULL,?,?,?,?) ON CONFLICT(id) DO UPDATE SET state='running',owner=excluded.owner,lease_until=excluded.lease_until`).run(id, scene, humanId, token, now + 180_000, now);
      return { token };
    }).immediate();
  }
  finish(id: string, token: string, result: ParticipationResult): void {
    const update = this.db.prepare("UPDATE external_discussion_attempts SET state='done',result=?,lease_until=0 WHERE id=? AND owner=? AND state='running'").run(JSON.stringify(result), id, token);
    if (update.changes !== 1) throw new Error("discussion lease lost");
  }
  fail(id: string, token: string): void {
    this.db.prepare("UPDATE external_discussion_attempts SET state='failed',lease_until=0 WHERE id=? AND owner=?").run(id, token);
  }
}
