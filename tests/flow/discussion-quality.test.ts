import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";

const directory = path.resolve(".tmp/flow-test/discussion-quality");
fs.mkdirSync(directory, { recursive: true });
process.env.DATABASE_PATH = path.join(directory, "discutere.db");
for (const suffix of ["", "-shm", "-wal"]) fs.rmSync(`${process.env.DATABASE_PATH}${suffix}`, { force: true });

const { _resetFlowDb, getFlowDb } = await import("../../src/flow/db/connection.js");
const { assessAndSaveDiscussionQuality, getDiscussionQuality } = await import(
  "../../src/flow/discussion-quality.js"
);
_resetFlowDb();
const db = getFlowDb();
db.prepare(
  `INSERT INTO discussion_paper
     (id, flow, session_id, theme, tags_json, mechanics_json, supplement, created_at, updated_at)
   VALUES (?, 'discussion', ?, '施策', '[]', '[]', '', ?, ?)`
).run("paper-quality", "session-quality", Date.now(), Date.now());
db.prepare(
  `INSERT INTO flow_conclusion
     (session_id, paper_id, summary, aufhebung_json, top_utterance_ids_json, concluded, created_at)
   VALUES (?, ?, '結論', '[]', '[]', 1, ?)`
).run("session-quality", "paper-quality", Date.now());

const llm = {
  async invoke() {
    return {
      ok: true as const,
      text: JSON.stringify({
        informationSufficiency: {
          score: 72,
          rationale: "仕様と対象はあるが成功指標が不足",
          missing: ["成功指標"],
        },
        meaningfulness: {
          score: 88,
          rationale: "反証とトレードオフを比較した",
          missing: [],
        },
      }),
    };
  },
};
const result = await assessAndSaveDiscussionQuality({
  sessionId: "session-quality",
  theme: "オンボーディング施策",
  paper: "施策Aと施策B",
  conclusion: "施策Aを小規模検証する",
  utterances: [{ personaName: "慎重派", text: "離脱率を先に測るべき" }],
  llm,
});
assert.equal(result.status, "scored");
if (result.status === "scored") {
  assert.equal(result.overallScore, 80);
  assert.deepEqual(result.informationSufficiency.missing, ["成功指標"]);
}
assert.deepEqual(getDiscussionQuality("session-quality"), result);

_resetFlowDb();
console.log("  [ok] discussion quality: AI scores persisted with conclusion");
