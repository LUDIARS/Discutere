/**
 * per-session fire counter store (restart-recovery).
 *
 * persona-engine の safety cap (maxFiresPerSession / maxFiresPerRulePerSession) は
 * 「session 内で何回発火したか」 をカウントして判定する。これが engine.ts の
 * in-memory Map にしか無いと、再起動でカウントが 0 に戻り cap が再起動越しに
 * 効かなくなる。SQLite に逃がして再起動耐性を持たせる。
 *
 * - InMemorySessionFireStore: 従来挙動 (テスト / db 無し時の既定)。
 * - SqliteSessionFireStore: `session_rule_fires` テーブルに永続化 (本番)。
 *
 * sessionId が null の発火 (tick rule) はカウント対象外 (cooldown で十分) であり、
 * caller (engine) 側で null を弾く前提。本 store は常に有効な sessionId を受ける。
 */

import type Database from "better-sqlite3";

export interface SessionFireStore {
  /** session 内の総発火数 */
  total(sessionId: string): number;
  /** session 内の特定 rule の発火数 */
  ruleCount(sessionId: string, ruleId: string): number;
  /** 1 件発火を記録 (count + 1) */
  increment(sessionId: string, ruleId: string): void;
  /** session のカウンタを全消去 (議論クローズ時の cap 解放) */
  reset(sessionId: string): void;
}

export class InMemorySessionFireStore implements SessionFireStore {
  // Map<sessionId, Map<ruleId, count>>
  private readonly fires = new Map<string, Map<string, number>>();

  total(sessionId: string): number {
    const perRule = this.fires.get(sessionId);
    if (!perRule) return 0;
    let sum = 0;
    for (const v of perRule.values()) sum += v;
    return sum;
  }

  ruleCount(sessionId: string, ruleId: string): number {
    return this.fires.get(sessionId)?.get(ruleId) ?? 0;
  }

  increment(sessionId: string, ruleId: string): void {
    let perRule = this.fires.get(sessionId);
    if (!perRule) {
      perRule = new Map();
      this.fires.set(sessionId, perRule);
    }
    perRule.set(ruleId, (perRule.get(ruleId) ?? 0) + 1);
  }

  reset(sessionId: string): void {
    this.fires.delete(sessionId);
  }
}

export class SqliteSessionFireStore implements SessionFireStore {
  constructor(private readonly db: Database.Database) {}

  total(sessionId: string): number {
    const row = this.db
      .prepare("SELECT COALESCE(SUM(count), 0) AS total FROM session_rule_fires WHERE session_id = ?")
      .get(sessionId) as { total: number };
    return row.total;
  }

  ruleCount(sessionId: string, ruleId: string): number {
    const row = this.db
      .prepare("SELECT count FROM session_rule_fires WHERE session_id = ? AND rule_id = ?")
      .get(sessionId, ruleId) as { count: number } | undefined;
    return row?.count ?? 0;
  }

  increment(sessionId: string, ruleId: string): void {
    this.db
      .prepare(
        `INSERT INTO session_rule_fires (session_id, rule_id, count) VALUES (?, ?, 1)
         ON CONFLICT(session_id, rule_id) DO UPDATE SET count = count + 1`
      )
      .run(sessionId, ruleId);
  }

  reset(sessionId: string): void {
    this.db.prepare("DELETE FROM session_rule_fires WHERE session_id = ?").run(sessionId);
  }
}
