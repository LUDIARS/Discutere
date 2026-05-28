import fs from "node:fs";
import path from "node:path";
import assert from "node:assert/strict";

import { createCore } from "../../src/core/index.js";

function rmrf(p: string) {
  if (fs.existsSync(p)) fs.rmSync(p, { recursive: true, force: true });
}

const workDir = path.resolve(".tmp/core-test");
const dbPath = path.join(workDir, "discatier.kuzu");
const eventPath = path.join(workDir, "events.jsonl");
rmrf(workDir);
fs.mkdirSync(workDir, { recursive: true });

process.env.DISCATIER_KUZU_PATH = dbPath;

{
  const core = createCore(dbPath, eventPath);
  // migration re-run should be no-op
  core.close();
}
{
  const core = createCore(dbPath, eventPath);
  const ws = "ws-1";

  const personId = core.repos.person.create({ workspaceId: ws, name: "alice" });
  assert.ok(personId.length > 0);

  const sessionId = core.repos.session.create({ workspaceId: ws, title: "s1", startedAt: Date.now() });
  const uttId = core.repos.utterance.create({
    workspaceId: ws,
    sessionId,
    rawContent: "hello",
    postedAt: Date.now(),
  });

  const lines = fs.readFileSync(eventPath, "utf8").trim().split("\n");
  assert.ok(lines.length >= 3);

  const utt = core.repos.utterance.get(uttId) as any;
  assert.equal(utt.raw_content, "hello");

  core.repos.utterance.update(uttId, { workspaceId: ws, normalizedContent: "hello!" });
  assert.throws(() => {
    core.repos.utterance.update(uttId, { workspaceId: ws, normalizedContent: "x", rawContent: "rewritten" });
  });

  // vector
  core.vectors.registerEmbedding({ workspaceId: ws, nodeType: "utterance", nodeId: uttId, vector: [1, 0, 0] });
  core.vectors.registerEmbedding({ workspaceId: ws, nodeType: "utterance", nodeId: "u2", vector: [0, 1, 0] });
  const sims = core.vectors.searchSimilar({ workspaceId: ws, vector: [0.9, 0.1, 0], k: 1 });
  assert.equal(sims[0].nodeId, uttId);

  core.eventLog.rebuildFromEventStore();
  const sessions = core.repos.session.list(ws) as any[];
  assert.equal(sessions.length, 1);

  core.close();
}

console.log("core tests passed");
