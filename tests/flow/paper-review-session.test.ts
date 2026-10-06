/**
 * Discord ペーパーレビューの単一セッション管理テスト。
 * - 準備を二重に始めない (openPreparing)
 * - 準備中の返信は溜めて、レビュー中へ移るときに届いた順で取り出す
 * - フロー再提案は 1 セッション 1 回
 * - スレッドごとの直列実行 (前の task が終わってから次が走る / 失敗しても後続は走る)
 */

import assert from "node:assert/strict";

const s = await import("../../src/flow/paper-review-session.js");

// ── 準備の二重起動を防ぐ ──────────────────────────────────────────────────────

{
  s._resetReviewSessions();
  assert.equal(s.openPreparing("t1"), true, "初回は開ける");
  assert.equal(s.openPreparing("t1"), false, "同じスレッドで準備を二重に始めない");
  assert.equal(s.isPreparing("t1"), true);
  assert.equal(s.hasReviewSession("t1"), true);
  assert.equal(s.openPreparing("t2"), true, "別スレッドは独立");
  s.closeReviewSession("t1");
  assert.equal(s.hasReviewSession("t1"), false);
  assert.equal(s.openPreparing("t1"), true, "閉じた後は開き直せる");
  console.log("  [ok] openPreparing: 準備を二重に始めない");
}

// ── 準備中の返信を溜める ──────────────────────────────────────────────────────

{
  s._resetReviewSessions();
  assert.equal(s.bufferReply("t1", "x"), false, "セッションが無ければ溜めない");
  s.openPreparing("t1");
  assert.equal(s.bufferReply("t1", "メカニクスに周回を追加"), true);
  assert.equal(s.bufferReply("t1", "開始"), true);
  assert.deepEqual(s.markReviewing("t1"), ["メカニクスに周回を追加", "開始"], "届いた順で取り出す");
  assert.equal(s.isPreparing("t1"), false, "レビュー中へ移る");
  assert.equal(s.bufferReply("t1", "y"), false, "レビュー中は溜めない (その場で処理する)");
  assert.deepEqual(s.markReviewing("t1"), [], "取り出した分は消える");
  assert.deepEqual(s.markReviewing("t9"), [], "セッションが無くても (再起動後の再開) レビュー中として開く");
  assert.equal(s.hasReviewSession("t9"), true);
  console.log("  [ok] bufferReply / markReviewing: 準備中の返信を捨てない");
}

// ── フロー再提案は 1 回 ───────────────────────────────────────────────────────

{
  s._resetReviewSessions();
  assert.equal(s.claimRepropose("t1"), false, "セッションが無ければ出さない");
  s.openPreparing("t1");
  assert.equal(s.claimRepropose("t1"), true, "最初の 1 回は出す");
  assert.equal(s.claimRepropose("t1"), false, "同じセッションで 2 回目は出さない");
  console.log("  [ok] claimRepropose: 1 セッション 1 回");
}

// ── 直列実行 ──────────────────────────────────────────────────────────────────

{
  s._resetReviewSessions();
  const order: string[] = [];
  let release!: () => void;
  const gate = new Promise<void>((r) => (release = r));
  const first = s.runSerial("t1", async () => {
    order.push("first:start");
    await gate;
    order.push("first:end");
    return 1;
  });
  const second = s.runSerial("t1", async () => {
    order.push("second");
    return 2;
  });
  const other = s.runSerial("t2", async () => {
    order.push("other-thread");
    return 3;
  });
  assert.equal(await other, 3, "別スレッドは待たない");
  assert.deepEqual(order, ["first:start", "other-thread"], "同じスレッドの 2 件目は 1 件目が終わるまで走らない");
  release();
  assert.equal(await first, 1);
  assert.equal(await second, 2);
  assert.deepEqual(order, ["first:start", "other-thread", "first:end", "second"]);

  const failed = s.runSerial("t3", async () => {
    throw new Error("boom");
  });
  const after = s.runSerial("t3", async () => "ok");
  await assert.rejects(failed, /boom/, "失敗は呼び出し元へ返す");
  assert.equal(await after, "ok", "前の task が失敗しても後続は走る");
  console.log("  [ok] runSerial: スレッドごとに直列・失敗しても後続は走る");
}

s._resetReviewSessions();
console.log("paper-review-session tests: all passed");
