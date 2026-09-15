import type Database from "better-sqlite3";
import { z } from "zod";

const schema = z.object({
  schemaVersion: z.literal(1),
  source: z.enum(["steam", "youtube"]),
  generatedAt: z.number().finite(), expiresAt: z.number().finite(),
  windowStart: z.number().finite(), firstObservedAt: z.number().finite(), lastObservedAt: z.number().finite(),
  sampleCount: z.number().int().positive(), observedMonths: z.number().int().min(1).max(7),
  coverage: z.enum(["partial", "target-window-observed"]),
}).strict();
export type PersonaHistoryEvidence = z.infer<typeof schema>;

export function parseHistoryEvidence(value: unknown, now = Date.now()): PersonaHistoryEvidence | undefined {
  if (value === undefined) return undefined;
  const result = schema.parse(value);
  if (result.expiresAt <= now || result.generatedAt > now || result.expiresAt <= result.generatedAt ||
    result.expiresAt > result.generatedAt + 30 * 86400_000 || result.windowStart > result.firstObservedAt ||
    result.firstObservedAt > result.lastObservedAt || result.lastObservedAt > result.generatedAt) {
    throw new Error("Invalid or expired history evidence");
  }
  return result;
}

export function saveHistoryEvidence(db: Database.Database, userId: string, evidence?: PersonaHistoryEvidence): void {
  if (!evidence) return;
  db.prepare(`INSERT INTO flow_persona_history VALUES(?,?,?)
    ON CONFLICT(user_id) DO UPDATE SET expires_at=excluded.expires_at,payload=excluded.payload`)
    .run(userId, evidence.expiresAt, JSON.stringify(evidence));
}

/** Expired derived profiles cannot silently remain eligible for discussions. */
export function purgeHistoryPersonas(db: Database.Database, now = Date.now()): void {
  if (!db.prepare("SELECT 1 FROM flow_persona_history WHERE expires_at<=? LIMIT 1").get(now)) return;
  db.transaction(() => {
    db.prepare(`DELETE FROM flow_persona WHERE user_id IN
      (SELECT user_id FROM flow_persona_history WHERE expires_at<=?)`).run(now);
    db.prepare("DELETE FROM flow_persona_history WHERE expires_at<=?").run(now);
  })();
}
