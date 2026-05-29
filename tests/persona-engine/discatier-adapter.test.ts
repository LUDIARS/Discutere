/**
 * Discatier ↔ persona-engine の glue (`createDiscatierContextProvider`) を
 * 実 Discatier Core (KuzuClient + repos) で end-to-end 検証。
 *
 * - persona-engine が出す proposeHypothesis 要求が Discatier の hypotheses table に着地
 * - persona-engine が出す postUtterance 要求が Discatier の utterances table に着地
 * - listRecentGaps / listActiveHypotheses が Discatier の生データを正しく整形
 */

import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";

import { createCore } from "../../src/core/index.js";
import { createDiscatierContextProvider } from "../../src/discatier-engine-adapter/index.js";

const workDir = path.resolve(".tmp/pe-adapter");
fs.rmSync(workDir, { recursive: true, force: true });
fs.mkdirSync(workDir, { recursive: true });

const core = createCore(path.join(workDir, "db.kuzu"), path.join(workDir, "events.jsonl"));
const ws = "knowledge";

try {
  // seed: gap + session
  const gapId = core.repos.designGap.create({
    workspaceId: ws,
    title: "オンボーディング離脱",
    status: "open",
  });
  const sessionId = core.repos.session.create({
    workspaceId: ws,
    title: "design discussion",
    startedAt: Date.now(),
  });

  const ctx = createDiscatierContextProvider(core, { defaultSessionId: sessionId });

  // listRecentGaps
  const gaps = ctx.listRecentGaps(ws, 10);
  assert.equal(gaps.length, 1);
  assert.equal(gaps[0].id, gapId);
  assert.equal(gaps[0].title, "オンボーディング離脱");
  assert.deepEqual(gaps[0].hypothesisIds, []);
  console.log("ok listRecentGaps");

  // proposeHypothesis → DB に着地
  const propose = ctx.proposeHypothesis({
    workspaceId: ws,
    statement: "選択肢を 2 つに絞る",
    designGapId: gapId,
    byPersonaId: "advocate",
  });
  assert.ok(propose.id);

  const hypsAfterPropose = core.client.raw
    .prepare("SELECT id, statement, design_gap_id FROM hypotheses WHERE workspace_id = ?")
    .all(ws) as Array<{ id: string; statement: string; design_gap_id: string | null }>;
  assert.equal(hypsAfterPropose.length, 1);
  assert.equal(hypsAfterPropose[0].statement, "選択肢を 2 つに絞る");
  assert.equal(hypsAfterPropose[0].design_gap_id, gapId);
  console.log("ok proposeHypothesis lands in Discatier");

  // listActiveHypotheses
  const active = ctx.listActiveHypotheses(ws, 10);
  assert.equal(active.length, 1);
  assert.equal(active[0].statement, "選択肢を 2 つに絞る");
  assert.equal(active[0].designGapId, gapId);

  // gaps の hypothesisIds も更新されているか
  const gapsAfter = ctx.listRecentGaps(ws, 10);
  assert.deepEqual(gapsAfter[0].hypothesisIds, [propose.id]);
  console.log("ok listActiveHypotheses + gap.hypothesisIds reverse link");

  // postUtterance → DB に着地
  const utt = ctx.postUtterance({
    workspaceId: ws,
    sessionId,
    text: "そもそも 2 択でいいのか",
    byPersonaId: "sceptic",
  });
  assert.ok(utt.id);

  const utts = ctx.listRecentUtterances(ws, sessionId, 10);
  assert.equal(utts.length, 1);
  assert.equal(utts[0].rawContent, "そもそも 2 択でいいのか");

  const speakerRow = core.client.raw
    .prepare("SELECT speaker_id FROM utterances WHERE id = ?")
    .get(utt.id) as { speaker_id: string };
  assert.equal(speakerRow.speaker_id, "persona:sceptic");
  console.log("ok postUtterance + speaker_id includes persona prefix");

  // defaultSessionId fallback
  const utt2 = ctx.postUtterance({
    workspaceId: ws,
    sessionId: "",
    text: "default session fallback",
    byPersonaId: "advocate",
  });
  assert.ok(utt2.id);
  console.log("ok defaultSessionId fallback");
} finally {
  core.close();
}

console.log("discatier-adapter.test.ts: all passed");
