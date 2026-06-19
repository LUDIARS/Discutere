/**
 * 結論一覧キャッシュ (conclusion_cache) + 話題キーワード抽出のテスト。
 *  - topic-terms: タイトル → キーワード抽出
 *  - upsert / list / count
 *  - write-through が materialCount を温存しつつ discussionVolume を更新する
 *  - recomputeMaterialCounts (別途計算の書き戻し)
 */

import assert from "node:assert/strict";
import Database from "better-sqlite3";

import { applyFlowMigrations } from "../../src/flow/db/migrations.js";
import { topicTermsFromTitle } from "../../src/visualize/topic-terms.js";
import {
  upsertConclusionCache,
  writeThroughFlowConclusion,
  listCachedConclusions,
  conclusionCacheCount,
  ensureConclusionCacheFresh,
  recomputeMaterialCounts,
} from "../../src/visualize/conclusion-cache.js";
import type { ConclusionSummary } from "../../src/visualize/conclusions.js";

function makeDb(): Database.Database {
  const db = new Database(":memory:");
  applyFlowMigrations(db);
  return db;
}

function summary(over: Partial<ConclusionSummary> = {}): ConclusionSummary {
  return {
    gapId: "flow:s1",
    kind: "flow",
    sessionId: "s1",
    title: "「エルデンリング (Elden Ring)」は何が面白いのか",
    conclusion: "達成感",
    utteranceCount: 18,
    aufhebungCount: 3,
    updatedAt: 1000,
    ...over,
  };
}

// ── topic-terms ────────────────────────────────────────────────
{
  const t = topicTermsFromTitle("「エルデンリング (Elden Ring)」は何が面白いのか — 特に「探索」の観点から");
  assert.ok(t.includes("エルデンリング"), "和名を拾う");
  assert.ok(t.includes("Elden Ring"), "括弧内英名を拾う");
  assert.ok(!t.includes("探索"), "後続の「angle」は採らない (最初の「」のみ)");
  console.log("  [ok] topicTermsFromTitle: ゲーム名 (和名+英名)");

  const trend = topicTermsFromTitle("生成AIの普及は「創作」の価値と作り手の役割をどう変えるのか");
  assert.deepEqual(trend, ["創作"], "最初の「」をキーワードにする");

  const noQuote = topicTermsFromTitle("サブスクリプション疲れ — 所有から利用への移行は本当に幸福度を上げたのか");
  assert.ok(noQuote.includes("サブスクリプション疲れ"), "「」無しはダッシュ手前をトークン化");
  console.log("  [ok] topicTermsFromTitle: トレンド議題");
}

// ── upsert / list / count ──────────────────────────────────────
{
  const db = makeDb();
  assert.equal(conclusionCacheCount(db), 0, "初期は空");

  upsertConclusionCache(db, summary({ gapId: "flow:a", sessionId: "a", updatedAt: 100 }), {
    discussionVolume: 18,
    materialCount: 42,
  });
  upsertConclusionCache(db, summary({ gapId: "flow:b", sessionId: "b", updatedAt: 200 }), {
    discussionVolume: 24,
    materialCount: 7,
  });

  assert.equal(conclusionCacheCount(db), 2, "2 件");
  const list = listCachedConclusions(db, 100);
  assert.equal(list.length, 2);
  assert.equal(list[0].gapId, "flow:b", "新しい順 (updatedAt DESC)");
  assert.equal(list[0].discussionVolume, 24, "discussionVolume 反映");
  assert.equal(list[0].materialCount, 7, "materialCount 反映");
  assert.equal(list[1].materialCount, 42);
  console.log("  [ok] upsert / list (新しい順) / count");

  // limit
  assert.equal(listCachedConclusions(db, 1).length, 1, "limit 効く");
  db.close();
}

// ── write-through が materialCount を温存する ──────────────────
{
  const db = makeDb();
  // build が materialCount=50 を入れた後…
  upsertConclusionCache(db, summary({ gapId: "flow:s1", updatedAt: 100 }), {
    discussionVolume: 10,
    materialCount: 50,
  });
  // …議論が再収束して write-through (materialCount 未算出, 発話数増)
  writeThroughFlowConclusion(db, {
    sessionId: "s1",
    title: "「エルデンリング」は何が面白いのか",
    conclusion: "達成感と探索",
    utteranceCount: 30,
    aufhebungCount: 5,
    updatedAt: 300,
  });
  const row = listCachedConclusions(db, 10)[0];
  assert.equal(row.discussionVolume, 30, "discussionVolume は write-through で更新");
  assert.equal(row.utteranceCount, 30, "utteranceCount も更新");
  assert.equal(row.materialCount, 50, "materialCount は重い算出値を温存 (-1 で上書きしない)");
  assert.equal(row.conclusion, "達成感と探索", "conclusion 更新");
  assert.equal(row.updatedAt, 300);
  console.log("  [ok] write-through: discussionVolume 更新 + materialCount 温存");
}

// ── recomputeMaterialCounts (別途計算) ─────────────────────────
{
  const db = makeDb();
  upsertConclusionCache(db, summary({ gapId: "flow:x", title: "「モンスト」は何が面白いのか", updatedAt: 1 }));
  upsertConclusionCache(db, summary({ gapId: "flow:y", title: "「原神」は何が面白いのか", updatedAt: 2 }));
  // 初期は未算出 (-1)
  assert.equal(listCachedConclusions(db, 10).every((r) => r.materialCount === -1), true, "初期 -1");

  const updated = recomputeMaterialCounts(db, (s) => (s.title.includes("モンスト") ? 123 : 9));
  assert.equal(updated, 2, "2 行算出");
  const byId = Object.fromEntries(listCachedConclusions(db, 10).map((r) => [r.gapId, r.materialCount]));
  assert.equal(byId["flow:x"], 123, "モンスト=123");
  assert.equal(byId["flow:y"], 9, "原神=9");

  // 失敗 (throw) する 1 件は skip し他は通す
  upsertConclusionCache(db, summary({ gapId: "flow:z", title: "「壊れ」", updatedAt: 3 }));
  const updated2 = recomputeMaterialCounts(db, (s) => {
    if (s.gapId === "flow:z") throw new Error("boom");
    return 1;
  });
  assert.equal(updated2, 3, "対象行数は数える");
  const zRow = listCachedConclusions(db, 10).find((r) => r.gapId === "flow:z");
  assert.equal(zRow?.materialCount, -1, "throw した行は据え置き (-1)");
  console.log("  [ok] recomputeMaterialCounts: 算出 + 1件失敗 skip");
}

// ── ensureConclusionCacheFresh: 過去分を KG 非依存で追いつかせる ──
{
  const db = makeDb();
  const now = 500;
  // flow_conclusion を 2 件直接投入 (= 過去に収束済だがキャッシュ未バックフィル)
  const insPaper = db.prepare(
    `INSERT INTO discussion_paper (id, flow, session_id, theme, created_at, updated_at)
     VALUES (?, 'discussion', ?, ?, ?, ?)`
  );
  const insConc = db.prepare(
    `INSERT INTO flow_conclusion (session_id, paper_id, summary, aufhebung_json, top_utterance_ids_json, concluded, created_at)
     VALUES (?, ?, ?, '[]', '[]', 1, ?)`
  );
  insPaper.run("p1", "s1", "「モンスト」は何が面白いのか", now, now);
  insConc.run("s1", "p1", "【収束】沼の併存", now);
  insPaper.run("p2", "s2", "「原神」は何が面白いのか", now + 1, now + 1);
  insConc.run("s2", "p2", "【収束】報酬系の対比", now + 1);

  assert.equal(conclusionCacheCount(db), 0, "キャッシュ未構築");
  ensureConclusionCacheFresh(db);
  assert.equal(conclusionCacheCount(db), 2, "過去 flow 結論を追いつかせた");
  const list = listCachedConclusions(db, 10);
  assert.equal(list[0].sessionId, "s2", "新しい順");
  assert.equal(list[0].materialCount, -1, "materialCount は未算出 (-1)");

  // 既に追いついていれば何もしない (件数不変)
  ensureConclusionCacheFresh(db);
  assert.equal(conclusionCacheCount(db), 2, "冪等");

  // 新規収束が write-through された後でも、過去分は痩せない
  insPaper.run("p3", "s3", "「テトリス」", now + 2, now + 2);
  insConc.run("s3", "p3", "【収束】普遍性", now + 2);
  writeThroughFlowConclusion(db, {
    sessionId: "s3",
    title: "「テトリス」",
    conclusion: "普遍性",
    utteranceCount: 18,
    aufhebungCount: 2,
    updatedAt: now + 2,
  });
  ensureConclusionCacheFresh(db);
  assert.equal(conclusionCacheCount(db), 3, "write-through 後も全件揃う");
  console.log("  [ok] ensureConclusionCacheFresh: 過去分バックフィル + 冪等");
}

console.log("conclusion-cache tests: all passed");
