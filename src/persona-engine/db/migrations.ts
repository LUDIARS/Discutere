/**
 * persona-engine DB migration (self-contained).
 *
 * consumer は `applyPersonaEngineMigrations(db)` を呼ぶだけで必要 table が揃う。
 * `_pe_migrations` を独自に持ち、 consumer 側の `_migrations` とぶつからない。
 */

import type Database from "better-sqlite3";

const MIGRATIONS: Array<{ id: string; sql: string[] }> = [
  {
    id: "pe_0001_initial",
    sql: [
      `CREATE TABLE IF NOT EXISTS personas (
        id TEXT PRIMARY KEY,
        name TEXT NOT NULL,
        display_name TEXT NOT NULL DEFAULT '',
        description TEXT NOT NULL,
        traits TEXT NOT NULL,
        speech_style TEXT NOT NULL,
        skill_template TEXT NOT NULL DEFAULT '',
        learned_notes TEXT NOT NULL DEFAULT '[]',
        created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL
      )`,
      `CREATE TABLE IF NOT EXISTS persona_assignments (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        persona_id TEXT NOT NULL,
        session_id TEXT NOT NULL,
        assigned_at INTEGER NOT NULL,
        released_at INTEGER
      )`,
      `CREATE TABLE IF NOT EXISTS persona_feedback (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        persona_id TEXT NOT NULL,
        session_id TEXT,
        ts INTEGER NOT NULL,
        kind TEXT NOT NULL,
        delta TEXT NOT NULL,
        detail TEXT
      )`,
      `CREATE TABLE IF NOT EXISTS rules (
        id TEXT PRIMARY KEY,
        description TEXT,
        trigger_type TEXT NOT NULL,
        tick_sec INTEGER,
        event_kind TEXT,
        conditions TEXT NOT NULL DEFAULT '[]',
        instructions TEXT NOT NULL,
        target TEXT,
        cooldown_sec INTEGER NOT NULL DEFAULT 60,
        last_fired_at INTEGER,
        enabled INTEGER NOT NULL DEFAULT 1,
        added_at INTEGER NOT NULL,
        added_by TEXT NOT NULL,
        removed_at INTEGER,
        removed_by TEXT,
        removed_reason TEXT
      )`,
      `CREATE TABLE IF NOT EXISTS rule_log (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        ts INTEGER NOT NULL,
        rule_id TEXT,
        action TEXT NOT NULL,
        detail TEXT,
        actor TEXT NOT NULL
      )`,
      `CREATE INDEX IF NOT EXISTS idx_persona_assignment_session ON persona_assignments(session_id, released_at)`,
      `CREATE INDEX IF NOT EXISTS idx_rule_log_ts ON rule_log(ts)`,
    ],
  },
];

export function applyPersonaEngineMigrations(db: Database.Database): void {
  db.exec(
    `CREATE TABLE IF NOT EXISTS _pe_migrations (id TEXT PRIMARY KEY, applied_at INTEGER NOT NULL)`
  );
  db.exec("BEGIN");
  try {
    for (const m of MIGRATIONS) {
      const exists = db
        .prepare("SELECT 1 FROM _pe_migrations WHERE id = ?")
        .get(m.id) as { 1: number } | undefined;
      if (exists) continue;
      for (const stmt of m.sql) {
        try {
          db.exec(stmt);
        } catch {
          /* idempotent: already applied */
        }
      }
      db.prepare("INSERT INTO _pe_migrations (id, applied_at) VALUES (?, ?)")
        .run(m.id, Date.now());
    }
    db.exec("COMMIT");
  } catch (e) {
    db.exec("ROLLBACK");
    throw e;
  }
}
