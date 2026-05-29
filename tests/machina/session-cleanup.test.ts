/**
 * mode-state TTL cleanup テスト (PR-C / MISSING #4-7).
 */

import assert from "node:assert/strict";

import {
  __resetAllSessions,
  cleanupOnce,
  taskSessionStore,
} from "../../src/machina/mode-state.js";

__resetAllSessions();

// 1. 新しい session を 1 件作る
const s = taskSessionStore.create({
  monitorId: "m1",
  workspaceId: "w1",
  platform: "slack",
  channelId: "ch1",
  threadKey: "t1",
  status: "hearing",
  messages: [],
  classification: null,
});
assert.ok(s.id);

// 2. cleanup with huge TTL → 削除されない
const r1 = cleanupOnce({ taskTtlMs: 60_000, discussionTtlMs: 60_000 });
assert.equal(r1.removedTaskSessions, 0);
const stillThere = taskSessionStore.findActiveByThread("m1", "t1");
assert.ok(stillThere, "fresh session should survive");
console.log("ok fresh session not removed");

// 3. updatedAt を強制的に過去にして cleanup → 削除される
//    updatedAt は public 変更可能 (TypeScript readonly ではない)
(stillThere as { updatedAt: Date }).updatedAt = new Date(Date.now() - 3 * 60 * 60 * 1000);
const r2 = cleanupOnce({ taskTtlMs: 60 * 60 * 1000, discussionTtlMs: 60 * 60 * 1000 });
assert.equal(r2.removedTaskSessions, 1);
const gone = taskSessionStore.findActiveByThread("m1", "t1");
assert.equal(gone, undefined, "stale session should be removed");
console.log("ok stale session removed by cleanup");

console.log("session-cleanup.test.ts: all passed");
