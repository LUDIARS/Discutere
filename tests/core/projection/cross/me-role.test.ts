import fs from "node:fs";
import path from "node:path";
import assert from "node:assert/strict";
import { createCore } from "../../../../src/core/index.js";
import { submitMessage } from "../../../../src/core/projection/message-input.js";

const dir = path.resolve(".tmp/p6-me");
fs.rmSync(dir, { recursive: true, force: true });
fs.mkdirSync(dir, { recursive: true });
const core = createCore(path.join(dir, "db.kuzu"), path.join(dir, "events.jsonl"));
const ws = "w1";
const sid = core.repos.session.create({ workspaceId: ws, title: "s", startedAt: Date.now() } as any);
const r = submitMessage({ core, workspaceId: ws, sessionId: sid, personId: "u1", rawContent: "/me role theorist" });
assert.equal(r.commandResult?.ok, true);
const p = core.repos.person.get("u1") as any;
assert.equal(p.role, "theorist");
core.close();
console.log("cross me-role passed");
