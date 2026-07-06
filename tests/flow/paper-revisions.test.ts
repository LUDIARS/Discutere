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
const {
  persistPaper,
  persistDraftPaper,
  getDraftPaper,
  updatePaperBody,
  getPaperBodyBySession,
  setPaperReviewInfo,
  getPaperReviewInfo,
} = await import("../../src/flow/discussion-paper.js");

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

// ── updatePaperBody / getPaperBodyBySession (ライブのペーパー更新) ────────────
{
  const sid = "paper-body-session";
  const paperId = persistPaper(
    { sessionId: sid, theme: "T", tags: [], mechanics: [], supplement: "", bodyMd: "# 議題\nT" },
    "discussion"
  );
  assert.equal(getPaperBodyBySession(sid), "# 議題\nT");
  updatePaperBody(paperId, "# 議題\nT\n\n# 議論の経過\n\n## ラウンド 1\nまとめ");
  assert.ok(getPaperBodyBySession(sid)?.includes("# 議論の経過"), "更新後の本文が読める");
  assert.equal(getPaperBodyBySession("no-such-session"), null);
}

// ── discussion paper review info: LLM check results are persisted for the edit UI ──
{
  const sid = "paper-review-info-session";
  persistDraftPaper(
    { sessionId: sid, theme: "T", tags: [], mechanics: [], supplement: "", bodyMd: "# 議題\nT" },
    "discussion"
  );
  const info = {
    voiceCount: 2,
    countCapped: false,
    samples: [{ content: "賛否が割れている", source: "learning" }],
    fixSuggestions: [{ title: "論点不足", reason: "対立軸が弱い", suggestedChange: "賛成/反対の論点を追記する" }],
    debatability: { degraded: false, debatable: false, message: "材料不足", recommendation: { flow: "learning" } },
    mechanicsKnowledge: {
      ok: false,
      confidence: "low",
      summary: "ゲームの基本ループが不足",
      knownMechanics: [],
      missingQuestions: ["主要メカニクスを補足する"],
    },
  };
  setPaperReviewInfo(sid, info);
  assert.deepEqual(getPaperReviewInfo(sid), info, "LLM チェック結果を review_info_json に保存して復元できる");
}

// ── persistDraftPaper / getDraftPaper / persistPaper upsert (draft → started) ──
{
  const sid = "draft-session";
  // ドラフト永続 → getDraftPaper で復元できる。
  const draftId = persistDraftPaper(
    { sessionId: sid, theme: "下書き議題", tags: ["開発"], mechanics: [{ name: "M", description: "d" }], supplement: "S", bodyMd: "# 議題\n下書き議題" },
    "discussion"
  );
  const row = getDraftPaper(sid);
  assert.ok(row, "draft が取得できる");
  assert.equal(row!.paperId, draftId);
  assert.equal(row!.theme, "下書き議題");
  assert.equal(row!.flow, "discussion");
  assert.deepEqual(row!.tags, ["開発"]);
  assert.equal(row!.mechanics.length, 1);

  // 同 session で再 persistDraftPaper は更新 (id 不変・status draft 維持)。
  const draftId2 = persistDraftPaper(
    { sessionId: sid, theme: "下書き議題(改)", tags: [], mechanics: [], supplement: "", bodyMd: "# 議題\n下書き議題(改)" },
    "discussion"
  );
  assert.equal(draftId2, draftId, "同 session は同一行を更新");
  assert.equal(getDraftPaper(sid)!.theme, "下書き議題(改)");

  // 確定 (persistPaper) で同 session を started に upsert → getDraftPaper は null。
  const startedId = persistPaper(
    { sessionId: sid, theme: "下書き議題(改)", tags: [], mechanics: [], supplement: "", bodyMd: "# 議題\n下書き議題(改)" },
    "discussion"
  );
  assert.equal(startedId, draftId, "started も同一行を upsert (重複行を作らない)");
  assert.equal(getDraftPaper(sid), null, "started 化したら draft では引けない");
}

console.log("paper-revisions.test.ts: all passed");
