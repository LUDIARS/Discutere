/**
 * 議論データ管理ツール (Fundamentum export) の単体テスト。
 * in-memory Foundation に差し替えてディスク非依存で確認する。
 */

import { Foundation } from "fundamentum";
import type { ConclusionDetail } from "../../src/visualize/conclusions.js";
import { buildDiscussionSnapshot } from "../../src/fundamentum-export/snapshot.js";
import { exportDiscussionToFundamentum } from "../../src/fundamentum-export/export.js";
import {
  listExportedDiscussions,
  getExportedDiscussionSnapshot,
  getDiscussionExportHistory,
} from "../../src/fundamentum-export/list.js";
import { setFundamentumStoreForTests } from "../../src/fundamentum-export/store.js";

let failed = 0;
function ok(cond: boolean, msg: string): void {
  if (cond) console.log(`ok ${msg}`);
  else {
    console.error(`FAIL ${msg}`);
    failed++;
  }
}

function makeDetail(overrides: Partial<ConclusionDetail> = {}): ConclusionDetail {
  return {
    gapId: "flow:session-1",
    kind: "flow",
    sessionId: "session-1",
    title: "テスト議論",
    conclusion: "結論本文",
    utteranceCount: 3,
    aufhebungCount: 1,
    updatedAt: 1700000000000,
    paper: { theme: "テーマ", tags: ["開発"], supplement: "", mechanics: [] },
    aufhebungen: ["止揚A"],
    topOpinions: [{ speaker: "論者#1", content: "意見1", score: 5, source: null, sourceUrl: null }],
    transcript: [{ speaker: "論者#1", content: "発話1", source: null, sourceUrl: null }],
    ...overrides,
  };
}

// --- snapshot.ts: 純関数 ---
{
  const snap = buildDiscussionSnapshot(makeDetail()) as Record<string, unknown>;
  ok(snap.discussionId === "flow:session-1", "snapshot に discussionId が入る");
  ok(JSON.stringify(snap).includes("結論本文"), "conclusion 本文が含まれる");
  const snapAgain = buildDiscussionSnapshot(makeDetail());
  ok(JSON.stringify(snap) === JSON.stringify(snapAgain), "同一 detail からは同一 snapshot (content-addressed dedup の前提)");
}

// --- export -> list -> get -> history: in-memory Foundation で往復 ---
{
  setFundamentumStoreForTests(Foundation.inMemory());

  const detail = makeDetail();
  const r1 = await exportDiscussionToFundamentum(detail);
  ok(r1.discussionId === detail.gapId, "export 結果の discussionId が一致");
  ok(r1.catalogVersion === 1, "初回 export で catalog version 1");

  const listed = await listExportedDiscussions();
  ok(listed.length === 1 && listed[0].discussionId === detail.gapId, "export 後に一覧へ 1 件出る");

  const snap = (await getExportedDiscussionSnapshot(detail.gapId)) as Record<string, unknown> | null;
  ok(snap !== null && snap.title === "テスト議論", "個別スナップショットが取得できる");

  const missing = await getExportedDiscussionSnapshot("nope");
  ok(missing === null, "未 export の議論は null");

  // 同一内容の再 export は no-op (content id 同一 → catalog version も据え置き)
  const r1b = await exportDiscussionToFundamentum(makeDetail());
  ok(r1b.contentId === r1.contentId, "同一内容の再 export は content id が変わらない (dedup)");
  ok(r1b.catalogVersion === 1, "同一内容の再 export は catalog version を進めない");

  // 内容を変えて再 export -> catalog version が進み、history に 2 版残る
  const r2 = await exportDiscussionToFundamentum(makeDetail({ conclusion: "更新後の結論" }));
  ok(r2.catalogVersion === 2, "内容が変わった再 export で version が進む");
  ok(r2.contentId !== r1.contentId, "内容が変われば content id も変わる (content-addressed)");

  const history = await getDiscussionExportHistory(detail.gapId);
  ok(history.length === 2, "history に 2 回の変化が残る");
  ok(history[0].contentId === r1.contentId && history[1].contentId === r2.contentId, "history が古い→新しい順");

  setFundamentumStoreForTests(undefined);
}

if (failed > 0) {
  console.error(`fundamentum-export tests: ${failed} failed`);
  process.exit(1);
}
console.log("fundamentum-export tests: all passed");
