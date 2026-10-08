import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";

const dir = fs.mkdtempSync(path.join(os.tmpdir(), "di-voice-choice-"));
process.env.DATABASE_PATH = path.join(dir, "flow.sqlite");
const { prepareVoiceChoice, readVoicePreparation, saveVoicePreparation, needsVoiceChoice, checkVoiceReceipt, voicePreparationNotice } = await import("../../src/flow/user-voices/preparation.js");
const { VoiceRequestFailure } = await import("../../src/flow/user-voices/request-client.js");
const { persistPaper, getPaperSnapshot } = await import("../../src/flow/discussion-paper.js");
const { forumSessionId, openRediscussion } = await import("../../src/flow/discord-flow-runs.js");
const { getFlowDb } = await import("../../src/flow/db/connection.js");

try {
  let submissions = 0;
  const request = { submit: async () => { submissions++; throw new Error("timeout after send"); }, status: async () => ({ accepted: true }) };
  const input = { sessionId: "s1", theme: "題", referenceCount: 0, priorKnowledge: false, request };
  const state = await prepareVoiceChoice(input);
  assert.equal(state.requestStatus, "unknown");
  await prepareVoiceChoice(input);
  assert.equal(submissions, 1, "結果不明を再送しない");
  state.choice = "wait";
  saveVoicePreparation("s1", state);
  assert.equal(readVoicePreparation("s1")?.choice, "wait");
  assert.equal(needsVoiceChoice(state), true);
  assert.equal((await checkVoiceReceipt("s1", request))?.requestStatus, "accepted");
  state.choice = "continue";
  assert.equal(needsVoiceChoice(state), false, "0 件でも明示選択で議論できる");
  const prior = await prepareVoiceChoice({ ...input, sessionId: "prior", priorKnowledge: true });
  assert.equal(prior.requestStatus, "not_needed");
  assert.equal(prior.referenceCount, 0, "事前知識を実ユーザーの声として数えない");
  const offline = await prepareVoiceChoice({ ...input, sessionId: "offline", request: { ...request, submit: async () => { throw new VoiceRequestFailure("down"); } } });
  assert.equal(offline.requestStatus, "unavailable");
  assert.match(voicePreparationNotice(offline), /停止中/);
  assert.match(voicePreparationNotice(offline), /外部の声なしで開始/);

  persistPaper({ sessionId: "thread", theme: "題", tags: [], mechanics: [], supplement: "", bodyMd: "# 議題\n題" });
  const original = getPaperSnapshot("thread");
  const next = openRediscussion("thread");
  assert.notEqual(next, "thread");
  assert.equal(forumSessionId("thread"), next);
  assert.equal(getPaperSnapshot(next!)?.status, "draft");
  assert.deepEqual(getPaperSnapshot("thread"), original, "再議論は元の記録を変更しない");
  assert.equal(openRediscussion("thread"), next, "連打でも草案は一つ");
} finally {
  getFlowDb().close();
  fs.rmSync(dir, { recursive: true, force: true });
}
