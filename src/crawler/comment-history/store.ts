import { mkdirSync } from "node:fs";
import { dirname } from "node:path";
import Database from "better-sqlite3";
import { historyStart, youtubeQuotaDay, YOUTUBE_REFRESH_MS } from "./window.js";
import type { HistoryJob, HistoryRecord, HistorySnapshot } from "./types.js";

/** Persistent checkpoints and quota reservations share the same transaction owner. */
export class HistoryStore {
  private readonly db: Database.Database;

  constructor(path: string) {
    mkdirSync(dirname(path), { recursive: true });
    this.db = new Database(path);
    try {
      this.db.pragma("journal_mode = WAL");
      this.db.exec(`
        CREATE TABLE IF NOT EXISTS history_record (
          source TEXT NOT NULL, id TEXT NOT NULL, posted_at INTEGER NOT NULL,
          fetched_at INTEGER NOT NULL, payload TEXT NOT NULL, PRIMARY KEY(source,id));
        CREATE TABLE IF NOT EXISTS history_job (key TEXT PRIMARY KEY, payload TEXT NOT NULL);
        CREATE TABLE IF NOT EXISTS history_quota (day TEXT PRIMARY KEY, units INTEGER NOT NULL);
        CREATE TABLE IF NOT EXISTS history_lease (id INTEGER PRIMARY KEY CHECK(id=1), owner TEXT NOT NULL, until_ms INTEGER NOT NULL);
      `);
    } catch (error) { this.db.close(); throw error; }
  }

  close(): void { this.db.close(); }

  acquire(owner: string, now: number): void {
    this.db.transaction(() => {
      const row = this.db.prepare("SELECT owner, until_ms FROM history_lease WHERE id=1").get() as
        { owner: string; until_ms: number } | undefined;
      if (row && row.owner !== owner && row.until_ms > now) throw new Error("History collector is already running");
      this.db.prepare("INSERT OR REPLACE INTO history_lease VALUES(1,?,?)").run(owner, now + 120_000);
    }).immediate();
  }

  release(owner: string): void {
    this.db.prepare("DELETE FROM history_lease WHERE id=1 AND owner=?").run(owner);
  }

  reserveQuota(now: number, units: number, limit: number): void {
    if (!Number.isSafeInteger(units) || units < 1 || !Number.isSafeInteger(limit) || limit < units) {
      throw new Error("Invalid YouTube quota allocation");
    }
    this.db.transaction(() => {
      const day = youtubeQuotaDay(now);
      const row = this.db.prepare("SELECT units FROM history_quota WHERE day=?").get(day) as { units: number } | undefined;
      if ((row?.units ?? 0) + units > limit) throw new Error("youtube_daily_budget_exhausted");
      this.db.prepare("INSERT INTO history_quota VALUES(?,?) ON CONFLICT(day) DO UPDATE SET units=excluded.units")
        .run(day, (row?.units ?? 0) + units);
    }).immediate();
  }

  jobs(): HistoryJob[] {
    return (this.db.prepare("SELECT payload FROM history_job ORDER BY key").all() as Array<{ payload: string }>)
      .map(row => JSON.parse(row.payload) as HistoryJob);
  }

  seed(job: HistoryJob): void {
    this.db.prepare("INSERT OR IGNORE INTO history_job VALUES(?,?)").run(job.key, JSON.stringify(job));
  }

  save(job: HistoryJob, records: HistoryRecord[] = [], children: HistoryJob[] = [], deleted: string[] = []): void {
    this.db.transaction(() => {
      const put = this.db.prepare(`INSERT INTO history_record VALUES(?,?,?,?,?)
        ON CONFLICT(source,id) DO UPDATE SET posted_at=excluded.posted_at,fetched_at=excluded.fetched_at,payload=excluded.payload`);
      for (const row of records) put.run(row.source, row.nativeId, row.postedAt, row.fetchedAt, JSON.stringify(row));
      for (const id of deleted) this.db.prepare("DELETE FROM history_record WHERE source='youtube-livechat' AND id=?").run(id);
      for (const child of children) this.seed(child);
      this.db.prepare("INSERT OR REPLACE INTO history_job VALUES(?,?)").run(job.key, JSON.stringify(job));
    }).immediate();
  }

  prune(now: number): void {
    this.db.prepare(`DELETE FROM history_record WHERE posted_at < ? OR
      COALESCE(json_extract(payload,'$.expiresAt'), fetched_at + ?) <= ?`).run(historyStart(now), YOUTUBE_REFRESH_MS, now);
    this.db.prepare("DELETE FROM history_quota WHERE day < ?").run(youtubeQuotaDay(now - 40 * 86400_000));
  }

  snapshot(now: number): HistorySnapshot {
    return this.db.transaction(() => this.readSnapshot(now))();
  }

  private readSnapshot(now: number): HistorySnapshot {
    this.prune(now);
    const records = (this.db.prepare("SELECT payload FROM history_record WHERE posted_at<=? ORDER BY source,id").all(now) as
      Array<{ payload: string }>).map(row => JSON.parse(row.payload) as HistoryRecord);
    const expiry = records.reduce((minimum, row) => Math.min(minimum, row.expiresAt), now + YOUTUBE_REFRESH_MS);
    return { schemaVersion: 1, generatedAt: now, windowStart: historyStart(now),
      expiresAt: expiry, coverage: this.jobs(), records };
  }
}
