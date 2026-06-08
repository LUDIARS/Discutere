import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";

import { openAttributionStore } from "../../../src/crawler/sources/attribution-store.js";

const workDir = path.resolve(".tmp/crawler-attribution");
fs.rmSync(workDir, { recursive: true, force: true });
fs.mkdirSync(workDir, { recursive: true });

const store = openAttributionStore(path.join(workDir, ".attribution.sqlite"));

// --- 1. set → get (出所メタの保持) ---
store.set({
  utteranceId: "ext:youtube:c1",
  source: "youtube",
  sourceUrl: "https://youtu.be/abc?lc=c1",
  nativeId: "c1",
  gameSlug: "hollow-knight",
  at: 1000,
});
const got = store.get("ext:youtube:c1");
assert.ok(got, "保存した行が引ける");
assert.equal(got!.source, "youtube");
assert.equal(got!.sourceUrl, "https://youtu.be/abc?lc=c1");
assert.equal(got!.gameSlug, "hollow-knight");
assert.equal(store.get("missing"), null, "未保存は null (内部発話など)");
console.log("ok attribution set/get");

// --- 2. upsert (同 utterance は上書き) ---
store.set({ utteranceId: "ext:youtube:c1", source: "youtube", sourceUrl: "https://updated", nativeId: "c1", gameSlug: "hk", at: 2000 });
assert.equal(store.get("ext:youtube:c1")!.sourceUrl, "https://updated", "upsert で上書き");
console.log("ok attribution upsert");

// --- 3. listBySource (source / gameSlug フィルタ) ---
store.set({ utteranceId: "ext:steam:r1", source: "steam", sourceUrl: "https://s/r1", nativeId: "r1", gameSlug: "hk", at: 3000 });
store.set({ utteranceId: "ext:steam:r2", source: "steam", sourceUrl: "https://s/r2", nativeId: "r2", gameSlug: "celeste", at: 4000 });
const steamAll = store.listBySource("steam");
assert.equal(steamAll.length, 2);
assert.equal(steamAll[0].utteranceId, "ext:steam:r2", "新しい順 (imported_at desc)");
const steamHk = store.listBySource("steam", "hk");
assert.equal(steamHk.length, 1);
assert.equal(steamHk[0].nativeId, "r1");
console.log("ok attribution listBySource");

// --- 4. countsBySource (件数 + gameSlugs + 直近時刻) ---
const counts = store.countsBySource();
const steamCount = counts.find((c) => c.source === "steam");
assert.ok(steamCount);
assert.equal(steamCount!.count, 2);
assert.deepEqual(new Set(steamCount!.gameSlugs), new Set(["hk", "celeste"]));
assert.equal(steamCount!.lastImportedAt, 4000);
console.log("ok attribution countsBySource");

store.close();
console.log("attribution-store.test.ts: all passed");
