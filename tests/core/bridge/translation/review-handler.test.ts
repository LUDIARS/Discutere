import fs from "node:fs";
import path from "node:path";
import assert from "node:assert/strict";
import { createCore } from "../../../../src/core/index.js";
import { approveProposal, rejectProposal, reviseProposal } from "../../../../src/core/bridge/translation/review-handler.js";

const dir = path.resolve(".tmp/p3-review");
fs.rmSync(dir, { recursive: true, force: true });
fs.mkdirSync(dir, { recursive: true });
const core = createCore(path.join(dir, "db.kuzu"), path.join(dir, "events.jsonl"));
const ws = "w1";
const sid = core.repos.session.create({ workspaceId: ws, title: "s", startedAt: Date.now(), mode: "emotion" });
const uid = core.repos.utterance.create({ workspaceId: ws, sessionId: sid, rawContent: "fun", postedAt: Date.now() });
const now = Date.now();
core.client.raw.prepare("INSERT INTO translation_proposals (id,workspace_id,utterance_id,proposal_type,target_name,confidence,pending_review,status,payload_json,created_at,updated_at) VALUES ('p1', ?, ?, 'affect', 'fun', 0.8, 1, 'pending', '{}', ?, ?)").run(ws, uid, now, now);
approveProposal(core, "p1");
let p = core.client.raw.prepare("SELECT * FROM translation_proposals WHERE id='p1'").get() as any;
assert.equal(p.status, "approved");

core.client.raw.prepare("INSERT INTO translation_proposals (id,workspace_id,utterance_id,proposal_type,target_name,confidence,pending_review,status,payload_json,created_at,updated_at) VALUES ('p2', ?, ?, 'affect', 'sad', 0.6, 1, 'pending', '{}', ?, ?)").run(ws, uid, now, now);
reviseProposal(core, "p2", "melancholy");
p = core.client.raw.prepare("SELECT * FROM translation_proposals WHERE id='p2'").get() as any;
assert.equal(p.status, "revised");

core.client.raw.prepare("INSERT INTO translation_proposals (id,workspace_id,utterance_id,proposal_type,target_name,confidence,pending_review,status,payload_json,created_at,updated_at) VALUES ('p3', ?, ?, 'mechanic', 'dash', 0.6, 1, 'pending', '{}', ?, ?)").run(ws, uid, now, now);
rejectProposal(core, "p3");
p = core.client.raw.prepare("SELECT * FROM translation_proposals WHERE id='p3'").get() as any;
assert.equal(p.status, "rejected");

core.close();
console.log("review-handler passed");
