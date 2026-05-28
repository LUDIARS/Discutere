import fs from "node:fs";
import path from "node:path";
import assert from "node:assert/strict";
import { createCore } from "../../../src/core/index.js";
import { submitMessage } from "../../../src/core/projection/message-input.js";

const dir = path.resolve(".tmp/p2-emotion");
fs.rmSync(dir, { recursive: true, force: true });
fs.mkdirSync(dir, { recursive: true });
const core = createCore(path.join(dir, "db.kuzu"), path.join(dir, "events.jsonl"));
const ws = "w1";
const sid = core.repos.session.create({ workspaceId: ws, title: "s", startedAt: Date.now() });

let r = submitMessage({ core, workspaceId: ws, sessionId: sid, rawContent: "/scene cave" });
assert.equal(r.commandResult?.ok, true);
r = submitMessage({ core, workspaceId: ws, sessionId: sid, rawContent: "/done" });
assert.equal(r.commandResult?.ok, true);

const s = core.repos.session.get(sid) as any;
assert.ok(s.ended_at);
core.close();
console.log("emotion-handlers passed");
