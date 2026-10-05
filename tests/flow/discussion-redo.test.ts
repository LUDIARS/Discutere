/**
 * 途中で止まった議論のやり直し判定 (discussion-redo.ts) テスト。
 */

import assert from "node:assert/strict";
import { decideRedo, isRedoText } from "../../src/flow/discussion-redo.js";

// ── 合図の判定 ───────────────────────────────────────────────────────────────
assert.equal(isRedoText("再開"), true);
assert.equal(isRedoText("  やり直して "), true);
assert.equal(isRedoText("再議論お願い"), true);
assert.equal(isRedoText("もう一度"), true);
assert.equal(isRedoText("開始"), false, "承認 (開始) とは別");
assert.equal(isRedoText("この論点を再開したい部分がある"), false, "先頭一致のみ");
console.log("  [ok] discussion-redo: やり直しの合図を判定");

// ── やり直せる条件 ───────────────────────────────────────────────────────────
const started = { status: "started", flow: "discussion" };
assert.deepEqual(decideRedo({ paper: started, concluded: false, running: false }), { ok: true });
assert.deepEqual(
  decideRedo({ paper: { status: "started", flow: "improvement" }, concluded: false, running: false }),
  { ok: true }
);
assert.deepEqual(decideRedo({ paper: null, concluded: false, running: false }), { ok: false, reason: "no-paper" });
assert.deepEqual(
  decideRedo({ paper: { status: "draft", flow: "discussion" }, concluded: false, running: false }),
  { ok: false, reason: "not-started" },
  "下書きはレビュー再開の経路"
);
assert.deepEqual(decideRedo({ paper: started, concluded: true, running: false }), { ok: false, reason: "concluded" });
assert.deepEqual(decideRedo({ paper: started, concluded: false, running: true }), { ok: false, reason: "running" });
assert.deepEqual(
  decideRedo({ paper: { status: "started", flow: "sparring" }, concluded: false, running: false }),
  { ok: false, reason: "unsupported-flow" }
);
console.log("  [ok] discussion-redo: 開始済み・結論なし・進行中でない議論だけやり直す");
