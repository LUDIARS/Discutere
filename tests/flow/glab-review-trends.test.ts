import assert from "node:assert/strict";
import Database from "better-sqlite3";
import { Hono } from "hono";

import { createGlabReviewTrendRoutes } from "../../src/api/glab-review-trend-routes.js";
import {
  analyzeStoredReviewTrends,
  readStoredSteamReviewSignals,
  steamAppIdFromSourceUrl,
} from "../../src/integrations/glab-review-trends.js";

const now = Date.parse("2026-08-25T00:00:00.000Z");
const integrationToken = "test-glab-review-trends-token-0001";

{
  const trends = analyzeStoredReviewTrends([
    { gameSlug: "alpha", appId: 10, postedAt: now - 60_000, reaction: "positive" },
    { gameSlug: "alpha", appId: 10, postedAt: now - 120_000, reaction: "negative" },
    { gameSlug: "beta", appId: 20, postedAt: now - 180_000, reaction: "positive" },
    { gameSlug: "old", appId: 30, postedAt: now - 8 * 24 * 60 * 60 * 1_000, reaction: "positive" },
  ], now);
  assert.deepEqual(trends, [
    {
      gameSlug: "alpha",
      appId: 10,
      recentReviewCount: 2,
      positivePercent: 50,
      latestReviewAt: "2026-08-24T23:59:00.000Z",
    },
    {
      gameSlug: "beta",
      appId: 20,
      recentReviewCount: 1,
      positivePercent: 100,
      latestReviewAt: "2026-08-24T23:57:00.000Z",
    },
  ]);
  console.log("  [ok] GLAB review trends aggregate stored Di records");
}

{
  assert.equal(
    steamAppIdFromSourceUrl("https://steamcommunity.com/profiles/1/recommended/1245620/"),
    1245620,
  );
  assert.equal(steamAppIdFromSourceUrl("https://example.com/recommended/1245620/"), null);
  console.log("  [ok] Steam App ID is derived only from stored official source URLs");
}

{
  const db = new Database(":memory:");
  db.exec(`
    CREATE TABLE utterances (id TEXT PRIMARY KEY, posted_at INTEGER NOT NULL);
    CREATE TABLE reactions (
      id TEXT PRIMARY KEY,
      utterance_id TEXT NOT NULL,
      reaction_type TEXT NOT NULL
    );
    CREATE TABLE utterance_exclusions (
      utterance_id TEXT PRIMARY KEY,
      reason TEXT,
      created_at INTEGER NOT NULL
    );
  `);
  db.prepare("INSERT INTO utterances (id, posted_at) VALUES (?, ?)").run("ext:steam:r1", now - 60_000);
  db.prepare("INSERT INTO utterances (id, posted_at) VALUES (?, ?)").run("ext:steam:r2", now - 120_000);
  db.prepare("INSERT INTO reactions (id, utterance_id, reaction_type) VALUES (?, ?, ?)")
    .run("ext:steam:r1:vote", "ext:steam:r1", "positive");
  db.prepare("INSERT INTO reactions (id, utterance_id, reaction_type) VALUES (?, ?, ?)")
    .run("later-negative", "ext:steam:r1", "negative");
  db.prepare("INSERT INTO utterance_exclusions (utterance_id, created_at) VALUES (?, ?)")
    .run("ext:steam:r2", now);
  const records = readStoredSteamReviewSignals(db, {
    listBySource: () => [
      {
        utteranceId: "ext:steam:r1",
        source: "steam",
        sourceUrl: "https://steamcommunity.com/profiles/1/recommended/10/",
        nativeId: "r1",
        gameSlug: "alpha",
        importedAt: now,
      },
      {
        utteranceId: "ext:steam:r2",
        source: "steam",
        sourceUrl: "https://steamcommunity.com/profiles/2/recommended/10/",
        nativeId: "r2",
        gameSlug: "alpha",
        importedAt: now,
      },
    ],
  });
  assert.deepEqual(records, [
    { gameSlug: "alpha", appId: 10, postedAt: now - 60_000, reaction: "positive" },
  ]);
  db.close();
  console.log("  [ok] GLAB review trends use importer votes and omit excluded utterances");
}

{
  const app = createGlabReviewTrendRoutes(() => [], () => "");
  const response = await app.request("/integrations/glab/review-trends");
  assert.equal(response.status, 503);
  assert.deepEqual(await response.json(), { ok: false, error: "review_trends_unavailable" });
  console.log("  [ok] GLAB review trends default to unavailable without a configured token");
}

{
  const app = new Hono();
  app.route("/api", createGlabReviewTrendRoutes(() => [{
    gameSlug: "alpha",
    appId: 10,
    recentReviewCount: 2,
    positivePercent: 50,
    latestReviewAt: "2026-08-24T23:59:00.000Z",
  }], () => integrationToken));
  const unauthorized = await app.request("/api/integrations/glab/review-trends");
  assert.equal(unauthorized.status, 401);
  const response = await app.request("/api/integrations/glab/review-trends", {
    headers: { authorization: `Bearer ${integrationToken}` },
  });
  assert.equal(response.status, 200);
  assert.equal(response.headers.get("cache-control"), "private, no-store");
  const body = await response.json();
  assert.equal(body.source, "discutere");
  assert.equal(body.data[0].gameSlug, "alpha");
  assert.equal(JSON.stringify(body).includes("content"), false);
  console.log("  [ok] GLAB review trend route exposes aggregate data only");
}

{
  const app = createGlabReviewTrendRoutes(
    () => { throw new Error("private database path"); },
    () => integrationToken,
  );
  const response = await app.request("/integrations/glab/review-trends", {
    headers: { authorization: `Bearer ${integrationToken}` },
  });
  assert.equal(response.status, 503);
  assert.deepEqual(await response.json(), { ok: false, error: "review_trends_unavailable" });
  console.log("  [ok] GLAB review trend failures do not expose internal details");
}
