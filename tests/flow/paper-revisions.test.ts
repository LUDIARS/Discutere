/**
 * paper-revisions: 本文 md の版履歴ストア (追記 / 一覧 / 戻す)。
 * DB を使うため DATABASE_PATH を専用にして _resetFlowDb で初期化する。
 */

import assert from "node:assert/strict";
import os from "node:os";
import path from "node:path";

process.env.DATABASE_PATH = path.join(os.tmpdir(), `discutere-paper-rev-${process.pid}.db`);
const { _resetFlowDb } = await import("../../src/flow/db/connection.js");
_resetFlowDb();
const { appendRevision, latestRevision, listRevisions, canRevert, revertLast } = await import(
  "../../src/flow/paper-revisions.js"
);

const SID = "rev-session-1";

// ── 追記 → 最新 / 一覧 ───────────────────────────────────────────────────────
{
  const r1 = appendRevision({ sessionId: SID, bodyMd: "v1", changeSummary: "初期草案", origin: "initial" });
  assert.equal(r1, 1, "最初の rev は 1");
  const r2 = appendRevision({ sessionId: SID, bodyMd: "v2", changeSummary: "編集", origin: "manual" });
  assert.equal(r2, 2);
  assert.equal(latestRevision(SID)?.bodyMd, "v2");
  assert.equal(latestRevision(SID)?.origin, "manual");
  assert.equal(listRevisions(SID).length, 2);
}

// ── 同一本文は追記しない (no-op で既存 rev) ──────────────────────────────────
{
  const same = appendRevision({ sessionId: SID, bodyMd: "v2", changeSummary: "重複", origin: "manual" });
  assert.equal(same, 2, "直前と同一本文は追記せず既存 rev を返す");
  assert.equal(listRevisions(SID).length, 2, "件数は増えない");
}

// ── 戻す: 1 手前の本文を新リビジョンとして積み直す ──────────────────────────
{
  assert.equal(canRevert(SID), true);
  const reverted = revertLast(SID);
  assert.equal(reverted?.bodyMd, "v1", "1 手前 (v1) の本文に戻る");
  assert.equal(reverted?.origin, "revert");
  assert.equal(listRevisions(SID).length, 3, "履歴は失わず前進 (rev3 を積む)");
  assert.equal(latestRevision(SID)?.bodyMd, "v1");
}

// ── 履歴 1 つだけは戻せない ───────────────────────────────────────────────────
{
  const sid2 = "rev-session-2";
  appendRevision({ sessionId: sid2, bodyMd: "only", changeSummary: "x", origin: "initial" });
  assert.equal(canRevert(sid2), false);
  assert.equal(revertLast(sid2), null);
}

console.log("paper-revisions.test.ts: all passed");
