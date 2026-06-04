import assert from "node:assert/strict";

import Database from "better-sqlite3";

import {
  applyReaction,
  ensureReactionTables,
  getOpinionScore,
  reactionWeight,
  recordPostedMessage,
  topScoredOpinions,
} from "../../src/discord-hook/reactions.js";

// 重み: 既定マップ + 未知は 1
assert.equal(reactionWeight("👍"), 1);
assert.equal(reactionWeight("💡"), 3);
assert.equal(reactionWeight("🤔"), 1);
assert.equal(reactionWeight("👍", { "👍": 5 }), 5);
console.log("ok reaction weight");

const db = new Database(":memory:");
ensureReactionTables(db);

recordPostedMessage(db, { messageId: "m1", targetId: "u1", targetKind: "utterance" });
recordPostedMessage(db, { messageId: "m2", targetId: "u2", targetKind: "utterance" });

// 未マッピングの message は null
assert.equal(applyReaction(db, { messageId: "unknown", emoji: "👍" }), null);

// m1 に 👍(1) + 💡(3) → u1 = 4
const r1 = applyReaction(db, { messageId: "m1", emoji: "👍" });
assert.equal(r1?.targetId, "u1");
assert.equal(r1?.score, 1);
const r2 = applyReaction(db, { messageId: "m1", emoji: "💡" });
assert.equal(r2?.score, 4, "重みが累積する");
assert.equal(getOpinionScore(db, "u1"), 4);

// m2 に 🔥(2)
applyReaction(db, { messageId: "m2", emoji: "🔥" });
assert.equal(getOpinionScore(db, "u2"), 2);
assert.equal(getOpinionScore(db, "u3"), 0, "未スコアは 0");

// 高スコア順
const top = topScoredOpinions(db, ["u1", "u2", "u3"], 5);
assert.deepEqual(
  top.map((t) => t.targetId),
  ["u1", "u2"],
  "スコア降順、 0 は除外"
);
console.log("ok reaction scoring + ranking");

db.close();
console.log("discord-hook reactions test: passed");
