import assert from "node:assert/strict";
import { test } from "node:test";
import { mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { HistoryStore } from "../../src/crawler/comment-history/store.js";
import { historyStart, youtubeQuotaDay } from "../../src/crawler/comment-history/window.js";
import { collectCommentPage, collectLivePage } from "../../src/crawler/comment-history/youtube-pages.js";
import type { HistorySettings, HistoryJob } from "../../src/crawler/comment-history/types.js";
import { collectSteamPage } from "../../src/crawler/comment-history/steam-pages.js";
import { parseHistoryEvidence, purgeHistoryPersonas } from "../../src/flow/persona-history-evidence.js";
import Database from "better-sqlite3";

const now = Date.parse("2026-09-15T10:00:00Z");
function withStore(action: (store: HistoryStore, settings: HistorySettings) => Promise<void> | void): Promise<void> {
  const directory = mkdtempSync(join(tmpdir(), "di-history-"));
  const database = join(directory, "history.sqlite");
  const store = new HistoryStore(database);
  const settings: HistorySettings = { database, youtubeApiKey: "fixture-key", youtubeDailyUnits: 100,
    liveChatRequestUnits: 5, youtubeRetentionDays: 30, maxRequests: 10, videos: [], steam: [] };
  return Promise.resolve().then(() => action(store, settings)).finally(() => {
    store.close(); rmSync(directory, { recursive: true });
  });
}
const response = (body: unknown): typeof fetch => async () => new Response(JSON.stringify(body), {
  headers: { "content-type": "application/json" },
});

test("six calendar months clamp month-end and quota follows Pacific midnight", () => {
  assert.equal(new Date(historyStart(Date.parse("2026-08-31T12:00:00Z"))).toISOString(), "2026-02-28T12:00:00.000Z");
  assert.notEqual(youtubeQuotaDay(Date.parse("2026-09-15T06:59:59Z")), youtubeQuotaDay(Date.parse("2026-09-15T07:00:00Z")));
});

test("quota is persisted independently of request success and reserves atomically", async () => withStore((store, settings) => {
  store.reserveQuota(now, 5, 5);
  const other = new HistoryStore(settings.database);
  try {
    assert.throws(() => other.reserveQuota(now, 1, 5), /budget_exhausted/);
    other.reserveQuota(now + 86400000, 1, 5);
  } finally { other.close(); }
}));

test("old parent still schedules recent replies without retaining unknown authors", async () => withStore(async (store, settings) => {
  const job: HistoryJob = { key: "comments:v", kind: "comments", target: "v", topic: "topic", startedAt: now, nextAt: now, status: "pending" };
  await collectCommentPage(job, settings, store, now, response({ items: [{ snippet: { totalReplyCount: 1,
    topLevelComment: { id: "old", snippet: { publishedAt: "2025-01-01T00:00:00Z", textOriginal: "old" } } } }] }));
  const reply = store.jobs().find(row => row.kind === "replies");
  assert.ok(reply);
  await collectCommentPage(reply, settings, store, now, response({ items: [{ id: "reply", snippet: {
    publishedAt: "2026-09-15T09:00:00Z", textDisplay: "fun", authorChannelId: { value: "UCauthor" },
  } }] }));
  assert.equal(store.snapshot(now).records.length, 1);
  assert.equal(store.snapshot(now).records[0].parentNativeId, "old");
  assert.equal(store.snapshot(now).records[0].content, "fun");
}));

test("live chat accepts arrivals during the request and persists the next allowed poll", async () => withStore(async (store, settings) => {
  const job: HistoryJob = { key: "livechat:v", kind: "livechat", target: "v", topic: "topic", liveChatId: "chat",
    startedAt: now, nextAt: now, status: "pending" };
  await collectLivePage(job, settings, store, now, response({ nextPageToken: "next", pollingIntervalMillis: 5000,
    items: [{ id: "m", authorDetails: { channelId: "UCauthor" }, snippet: { type: "textMessageEvent",
      publishedAt: new Date(now + 500).toISOString(), displayMessage: "hello" } }] }), () => now + 1000);
  assert.equal(store.snapshot(now + 1000).records.length, 1);
  assert.equal(store.jobs()[0].nextAt, now + 6000);
  assert.equal(store.jobs()[0].cursor, "next");
}));

test("Steam stops at the six-month boundary and replay does not duplicate records", async () => withStore(async store => {
  const job: HistoryJob = { key: "steam:10", kind: "steam", target: "10", topic: "game", startedAt: now, nextAt: now, status: "pending" };
  const page = response({ success: 1, cursor: "another", reviews: [
    { recommendationid: "recent", author: { steamid: "123" }, review: "fun", timestamp_created: (now - 1000) / 1000, voted_up: true },
    { recommendationid: "old", author: { steamid: "123" }, review: "old", timestamp_created: (historyStart(now) - 1000) / 1000, voted_up: false },
  ] });
  await collectSteamPage(job, store, now, page);
  await collectSteamPage(job, store, now, page);
  assert.equal(store.snapshot(now).records.length, 1);
  assert.equal(store.jobs()[0].status, "complete");
  assert.equal(store.jobs()[0].cursor, undefined);
}));

test("expired evidence is rejected and only expired derived profiles are removed", () => {
  const metadata = { schemaVersion: 1, source: "steam", generatedAt: now - 1000, expiresAt: now,
    windowStart: historyStart(now), firstObservedAt: now - 2000, lastObservedAt: now - 2000,
    sampleCount: 10, observedMonths: 1, coverage: "partial" };
  assert.throws(() => parseHistoryEvidence(metadata, now), /expired/);
  const db = new Database(":memory:");
  try {
    db.exec(`CREATE TABLE flow_persona(user_id TEXT PRIMARY KEY);
      CREATE TABLE flow_persona_history(user_id TEXT PRIMARY KEY, expires_at INTEGER, payload TEXT);
      INSERT INTO flow_persona VALUES('expired'),('fresh'),('registered');`);
    db.prepare("INSERT INTO flow_persona_history VALUES(?,?,?)").run("expired", now, "{}");
    db.prepare("INSERT INTO flow_persona_history VALUES(?,?,?)").run("fresh", now + 1000, "{}");
    purgeHistoryPersonas(db, now);
    assert.deepEqual(db.prepare("SELECT user_id FROM flow_persona ORDER BY user_id").all(),
      [{ user_id: "fresh" }, { user_id: "registered" }]);
  } finally { db.close(); }
});
