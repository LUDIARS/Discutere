import fs from "node:fs";
import path from "node:path";
import assert from "node:assert/strict";
import { createCore } from "../../../../src/core/index.js";
import { detectGaps } from "../../../../src/core/bridge/gap/detector.js";

const dir = path.resolve(".tmp/p4-detector");
fs.rmSync(dir, { recursive: true, force: true });
fs.mkdirSync(dir, { recursive: true });
const core = createCore(path.join(dir, "db.kuzu"), path.join(dir, "events.jsonl"));
const ws = "w1";

const mid = core.repos.mechanic.create({ workspaceId: ws, name: "parry" } as any);
core.client.raw.prepare("UPDATE mechanics SET intended_affect = ? WHERE id = ?").run("joy", mid);
for (let i = 0; i < 3; i += 1) {
  const sid = core.repos.session.create({ workspaceId: ws, title: `s${i}`, startedAt: Date.now(), mode: "emotion" } as any);
  const uid = core.repos.utterance.create({ workspaceId: ws, sessionId: sid, rawContent: "text", postedAt: Date.now() });
  core.client.raw.prepare("INSERT INTO translation_proposals (id,workspace_id,utterance_id,proposal_type,target_name,confidence,pending_review,status,payload_json,created_at,updated_at) VALUES (?, ?, ?, 'affect', 'anger', 0.8, 0, 'approved', '{}', ?, ?)").run(`p${i}`, ws, uid, Date.now(), Date.now());
}
const ids = detectGaps(core, ws);
assert.equal(ids.length, 1);
const again = detectGaps(core, ws);
assert.equal(again.length, 0);
core.close();
console.log("gap detector passed");
