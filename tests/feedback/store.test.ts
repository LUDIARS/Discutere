/**
 * GameFeedbackStore のダミー注入テスト (in-memory sqlite)。
 */
import assert from "node:assert/strict";
import Database from "better-sqlite3";

import { GameFeedbackStore } from "../../src/feedback/store.js";

const db = new Database(":memory:");
const store = new GameFeedbackStore(db);

// ダミー感想を注入
const t0 = 1_000_000;
store.add({ gameTitle: "Vampire Survivors", content: "ビルドの自由度が高くて中毒性ある", createdAt: t0 });
store.add({ gameTitle: "Vampire Survivors", content: "終盤の処理落ちが気になる", createdAt: t0 + 10 });
store.add({ gameTitle: "ワンダと巨像", content: "孤独感の演出が刺さる", createdAt: t0 + 20 });
// 空本文は無視
assert.equal(store.add({ gameTitle: "X", content: "   " }, ), null, "空本文は無視");

// 件数
assert.equal(store.countForGame("Vampire Survivors"), 2);
assert.equal(store.countForGame("ワンダと巨像"), 1);
console.log("ok add / countForGame");

// 直近取得 (新しい順)
const recent = store.recentForGame("Vampire Survivors", 12);
assert.equal(recent.length, 2);
assert.equal(recent[0].content, "終盤の処理落ちが気になる", "新しい順");
console.log("ok recentForGame (新しい順)");

// 匿名性: スキーマに author 列が無い
const cols = (db.prepare("PRAGMA table_info(game_feedback)").all() as Array<{ name: string }>).map((c) => c.name);
assert.ok(!cols.some((c) => /author|user|name/i.test(c)), `投稿者列を持たない (cols=${cols.join(",")})`);
console.log("ok anonymity (投稿者を保存しない)");

// ゲーム一覧 (件数降順)
const games = store.listGames();
assert.equal(games[0].gameTitle, "Vampire Survivors");
assert.equal(games[0].count, 2);
console.log("ok listGames");

db.close();
console.log("feedback/store.test.ts: all passed");
