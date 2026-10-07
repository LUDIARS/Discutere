/**
 * ペーパーレビュー中の返信への確認済みリアクション (paper-review-ack.ts) テスト。
 * - レビュー中 + 本文あり → ☑️ を付ける (承認用の ✅ とは別の絵文字)
 * - レビュー外 / 空返信 → 付けない
 * - 付与失敗は warn して処理を止めない
 */

import assert from "node:assert/strict";
import { ackPaperReviewReply, PAPER_REVIEW_ACK_EMOJI } from "../../src/discord-hook/paper-review-ack.js";

function fakeMessage(content: string, fail = false) {
  const reacted: string[] = [];
  return {
    reacted,
    msg: {
      content,
      react: async (emoji: string) => {
        if (fail) throw new Error("Missing Permissions");
        reacted.push(emoji);
      },
    },
  };
}

assert.notEqual(PAPER_REVIEW_ACK_EMOJI, "✅", "承認 (✅) と区別できる絵文字");

for (const content of ["ない", "はい"]) {
  const m = fakeMessage(content);
  assert.equal(await ackPaperReviewReply(m.msg, true), true);
  assert.deepEqual(m.reacted, [PAPER_REVIEW_ACK_EMOJI], "編集を伴わない回答にも受付チェック");
}

{
  const m = fakeMessage("開始");
  assert.equal(await ackPaperReviewReply({ ...m.msg, reference: { messageId: "other" } }, true), false);
  assert.deepEqual(m.reacted, [], "リプライには承認語でも反応しない");
}

{
  const m = fakeMessage("メカニクスにガチャを追加");
  assert.equal(await ackPaperReviewReply(m.msg, true), true);
  assert.deepEqual(m.reacted, [PAPER_REVIEW_ACK_EMOJI]);
  console.log("ok paper-review-ack: レビュー中の意見に確認リアクションを付ける");
}

{
  const outside = fakeMessage("ふつうの返信");
  assert.equal(await ackPaperReviewReply(outside.msg, false), false);
  assert.deepEqual(outside.reacted, []);
  const empty = fakeMessage("   ");
  assert.equal(await ackPaperReviewReply(empty.msg, true), false);
  assert.deepEqual(empty.reacted, []);
  console.log("ok paper-review-ack: レビュー外 / 空返信には付けない");
}

{
  const warns: string[] = [];
  const m = fakeMessage("開始", true);
  assert.equal(await ackPaperReviewReply(m.msg, true, (w) => warns.push(w)), true);
  assert.ok(warns.some((w) => w.includes("Missing Permissions")), "失敗理由を warn に出す");
  console.log("ok paper-review-ack: 付与失敗は warn して続行");
}
