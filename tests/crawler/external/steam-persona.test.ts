/**
 * steam-persona パイプラインの一気通貫テスト (fake ports + 一時 SQLite)。
 * 発見の閾値判定 / 増分打ち切りの永続化 / 横断検出の冪等性 / 集中収集の JSONL 形を固定する。
 */

import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";

import { mapUserReviewToUtterance } from "../../../src/crawler/steam-persona/collect.js";
import type { AppReviewEntry, SteamFetchPorts } from "../../../src/crawler/steam-persona/ports.js";
import { runSteamPersona } from "../../../src/crawler/steam-persona/runner.js";
import { openSteamPersonaStore } from "../../../src/crawler/steam-persona/store.js";

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "steam-persona-test-"));
const dbPath = path.join(tmp, "state.sqlite");
const outDir = path.join(tmp, "authors");

const A1 = "76561198000000001";
const A2 = "76561198000000002";

function appReview(id: string, steamId: string): AppReviewEntry {
  return { recommendationid: id, author: { steamid: steamId }, review: "r", timestamp_created: 1, voted_up: true };
}

/** app 111 / 222 の両方に A1 が、111 だけに A2 が投稿している世界。 */
const fetchedUserIds: string[] = [];
const stopSeen: Array<string | undefined> = [];
const ports: SteamFetchPorts = {
  fetchNewReleases: async () => [
    { appId: 111, title: "Game A", url: "https://store.steampowered.com/app/111/" },
    { appId: 222, title: "Game B", url: "https://store.steampowered.com/app/222/" },
    { appId: 333, title: "Tiny Game", url: "https://store.steampowered.com/app/333/" },
  ],
  fetchTotalReviews: async (appId) => (appId === 333 ? 50 : 500),
  fetchAppReviews: async ({ appId, stopAtRecommendationId }) => {
    stopSeen.push(stopAtRecommendationId);
    if (stopAtRecommendationId) return []; // 2 周目は新着なし
    return appId === 111
      ? [appReview("r1", A1), appReview("r2", A2), appReview("r-invalid", "../../unexpected")]
      : [appReview("r3", A1)];
  },
  fetchUserReviews: async ({ steamId }) => {
    fetchedUserIds.push(steamId);
    return [
      { steamId, appId: 111, recommended: true, text: "great", url: `https://steamcommunity.com/profiles/${steamId}/recommended/111/` },
      { steamId, appId: 999, recommended: false, text: "meh", url: `https://steamcommunity.com/profiles/${steamId}/recommended/999/` },
    ];
  },
};

const now = () => 1_755_000_000_000;

// ---- 1 周目: 発見 (閾値 200 超のみ) → 出現記録 → A1 を横断検出 → 集中収集
const first = await runSteamPersona(ports, { dbPath, outDir, minTotalReviews: 200, now });
assert.equal(first.scanned, 3);
assert.equal(first.trackedNew, 2); // 333 (50 reviews) は追跡しない
assert.equal(first.sightings, 3);
assert.equal(first.detected, 1); // A1 のみ (2 アプリ)
assert.equal(first.collectedAuthors, 1);
assert.equal(first.collectedReviews, 2);
assert.deepEqual(fetchedUserIds, [A1]); // A2 (1 アプリ) は集中収集しない
assert.equal(fs.existsSync(path.join(tmp, "unexpected.jsonl")), false); // 外部入力を出力パスに使わない

// JSONL がペルソナ元データ (ExternalUtterance 互換) になっている
const jsonl = fs.readFileSync(path.join(outDir, `${A1}.jsonl`), "utf8").trim().split("\n");
assert.equal(jsonl.length, 2);
const u = JSON.parse(jsonl[0]!);
assert.equal(u.source, "steam");
assert.equal(u.authorId, A1); // SteamID64 = persona アンカー
assert.equal(u.nativeId, `ur:${A1}:111`);
assert.equal(u.gameSlug, "steam-app-111");
assert.equal(u.signal.votedUp, true);

// ---- 2 周目: 冪等性 — 増分打ち切りが効き、再検出・再収集しない
const second = await runSteamPersona(ports, { dbPath, outDir, minTotalReviews: 200, now });
assert.equal(second.trackedNew, 0);
assert.equal(second.sightings, 0);
assert.equal(second.detected, 0);
assert.equal(second.collectedAuthors, 0);
assert.ok(stopSeen.slice(-2).every((s) => s === "r1" || s === "r3")); // 前回先頭 id で打ち切り

// ---- 集中収集: 取得失敗 (非公開プロフィール等) は 0 件で記録しループを継続する (spec §2-4)
{
  const failDb = path.join(tmp, "state-fail.sqlite");
  const failOut = path.join(tmp, "authors-fail");
  const seen: string[] = [];
  const failingPorts: SteamFetchPorts = {
    ...ports,
    fetchAppReviews: async ({ appId, stopAtRecommendationId }) => {
      if (stopAtRecommendationId) return [];
      return appId === 111 ? [appReview("f1", A1), appReview("f2", A2)] : [appReview("f3", A1), appReview("f4", A2)];
    },
    // A1 は非公開プロフィール想定で失敗、A2 は成功 → 失敗が後続を止めないことを固定する
    fetchUserReviews: async ({ steamId }) => {
      seen.push(steamId);
      if (steamId === A1) throw new Error("profile is private");
      return [{ steamId, appId: 111, recommended: true, text: "ok", url: "https://example.invalid/" }];
    },
  };

  const run = await runSteamPersona(failingPorts, { dbPath: failDb, outDir: failOut, minTotalReviews: 200, now });
  assert.deepEqual(seen, [A1, A2]); // A1 の失敗で打ち切られず A2 まで進む
  assert.equal(run.detected, 2);
  assert.equal(run.collectedAuthors, 2); // 失敗分も「収集試行済み」として記録
  assert.equal(run.failedAuthors, 1); // うち 1 人は取得失敗 (成功 0 件と区別できる)
  assert.equal(run.collectedReviews, 1); // A2 の 1 件のみ
  assert.equal(fs.existsSync(path.join(failOut, `${A1}.jsonl`)), false); // 失敗時は JSONL を作らない
  assert.equal(fs.existsSync(path.join(failOut, `${A2}.jsonl`)), true);

  // 失敗した投稿者も収集済みになり、次周で再試行されない (無限リトライしない)
  const again = await runSteamPersona(failingPorts, { dbPath: failDb, outDir: failOut, minTotalReviews: 200, now });
  assert.equal(again.collectedAuthors, 0);
  assert.equal(again.failedAuthors, 0);
}

// ---- mapUserReviewToUtterance: recommended 不明なら signal を持たない
const anon = mapUserReviewToUtterance(
  { steamId: A2, appId: 5, text: "t", url: "https://example.invalid/" },
  123
);
assert.equal(anon.signal, undefined);
assert.equal(anon.postedAt, 123);

// ---- store 単体: 検出済み投稿者は minApps を跨いでも二重登録されない
{
  const store = openSteamPersonaStore(path.join(tmp, "state2.sqlite"));
  store.addAuthorSighting({ appId: 1, steamId: A1, recommendationId: "x1", at: 1 });
  store.addAuthorSighting({ appId: 2, steamId: A1, recommendationId: "x2", at: 1 });
  assert.equal(store.detectCrossAuthors(2, 2).length, 1);
  store.addAuthorSighting({ appId: 3, steamId: A1, recommendationId: "x3", at: 3 });
  assert.equal(store.detectCrossAuthors(2, 4).length, 0); // 既検出
  store.close();
}

fs.rmSync(tmp, { recursive: true, force: true });
console.log("ok steam-persona pipeline");
