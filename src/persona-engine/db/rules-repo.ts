import type Database from "better-sqlite3";

import type { RuleLogRow, RuleRow, RuleSeed } from "../types.js";

function nowSec(): number {
  return Math.floor(Date.now() / 1000);
}

export class RulesRepo {
  constructor(private readonly db: Database.Database) {}

  // ─── rule CRUD ─────────────────────────────────

  insertOrIgnore(seed: RuleSeed, options: { addedBy?: string } = {}): void {
    const now = nowSec();
    this.db
      .prepare(
        `INSERT OR IGNORE INTO rules
          (id, description, trigger_type, tick_sec, event_kind, conditions,
           instructions, target, cooldown_sec, last_fired_at, enabled,
           added_at, added_by)
         VALUES (?, ?, ?, ?, ?, '[]', ?, ?, ?, NULL, 1, ?, ?)`
      )
      .run(
        seed.id,
        seed.description ?? null,
        seed.trigger_type,
        seed.tick_sec ?? null,
        seed.event_kind ?? null,
        seed.instructions,
        seed.target ?? null,
        seed.cooldown_sec ?? 60,
        now,
        options.addedBy ?? "seed"
      );
  }

  bulkSeed(seeds: RuleSeed[]): void {
    const insert = this.db.transaction((rows: RuleSeed[]) => {
      for (const row of rows) this.insertOrIgnore(row);
    });
    insert(seeds);
  }

  get(id: string): RuleRow | undefined {
    return this.db.prepare("SELECT * FROM rules WHERE id = ?").get(id) as
      | RuleRow
      | undefined;
  }

  list(filter?: {
    enabled?: boolean;
    trigger_type?: "tick" | "event";
  }): RuleRow[] {
    const where: string[] = [];
    const params: unknown[] = [];
    if (filter?.enabled !== undefined) {
      where.push("enabled = ?");
      params.push(filter.enabled ? 1 : 0);
    }
    if (filter?.trigger_type !== undefined) {
      where.push("trigger_type = ?");
      params.push(filter.trigger_type);
    }
    const sql = `SELECT * FROM rules${
      where.length > 0 ? " WHERE " + where.join(" AND ") : ""
    } ORDER BY id`;
    return this.db.prepare(sql).all(...params) as RuleRow[];
  }

  setLastFired(id: string, atSec: number): void {
    this.db
      .prepare("UPDATE rules SET last_fired_at = ? WHERE id = ?")
      .run(atSec, id);
  }

  setEnabled(id: string, enabled: boolean): void {
    this.db
      .prepare("UPDATE rules SET enabled = ? WHERE id = ?")
      .run(enabled ? 1 : 0, id);
  }

  remove(id: string, by: string, reason: string): void {
    this.db
      .prepare(
        "UPDATE rules SET enabled = 0, removed_at = ?, removed_by = ?, removed_reason = ? WHERE id = ?"
      )
      .run(nowSec(), by, reason, id);
  }

  // ─── log ──────────────────────────────────────

  log(input: {
    rule_id?: string | null;
    action: RuleLogRow["action"];
    actor: RuleLogRow["actor"];
    detail?: string | null;
  }): void {
    this.db
      .prepare(
        "INSERT INTO rule_log (ts, rule_id, action, actor, detail) VALUES (?, ?, ?, ?, ?)"
      )
      .run(
        nowSec(),
        input.rule_id ?? null,
        input.action,
        input.actor,
        input.detail ?? null
      );
  }

  recentLogs(limit = 20): RuleLogRow[] {
    return this.db
      .prepare("SELECT * FROM rule_log ORDER BY id DESC LIMIT ?")
      .all(limit) as RuleLogRow[];
  }
}
