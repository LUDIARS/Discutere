import fs from "node:fs";
import path from "node:path";
import assert from "node:assert/strict";
import { createCore } from "../../../../src/core/index.js";
import { runGapDetectionJob } from "../../../../src/core/jobs/gap-detection-job.js";

const dir = path.resolve(".tmp/p4-e2e");
fs.rmSync(dir, { recursive: true, force: true });
fs.mkdirSync(dir, { recursive: true });
const core = createCore(path.join(dir, "db.kuzu"), path.join(dir, "events.jsonl"));
const ws = "w1";

const mid = core.repos.mechanic.create({ workspaceId: ws, name: "dash" } as any);
core.client.raw.prepare("UPDATE mechanics SET intended_affect = ? WHERE id = ?").run("joy", mid);
for (let i = 0; i < 3; i += 1) {
  const sid = core.repos.session.create({ workspaceId: ws, title: `s${i}`, startedAt: Date.now(), mode: "emotion" } as any);
  const uid = core.repos.utterance.create({ workspaceId: ws, sessionId: sid, rawContent: "text", postedAt: Date.now() });
  core.client.raw.prepare("INSERT INTO translation_proposals (id,workspace_id,utterance_id,proposal_type,target_name,confidence,pending_review,status,payload_json,created_at,updated_at) VALUES (?, ?, ?, 'affect', 'anger', 0.8, 0, 'approved', '{}', ?, ?)").run(`pp${i}`, ws, uid, Date.now(), Date.now());
}

const detected = runGapDetectionJob(core, ws);
assert.equal(detected.length, 1);
core.close();
console.log("gap e2e passed");
