import fs from "node:fs";
import path from "node:path";
import assert from "node:assert/strict";
import { createCore } from "../../../src/core/index.js";
import { submitMessage } from "../../../src/core/projection/message-input.js";

const dir = path.resolve(".tmp/p2-axis1");
fs.rmSync(dir, { recursive: true, force: true });
fs.mkdirSync(dir, { recursive: true });
const core = createCore(path.join(dir, "db.kuzu"), path.join(dir, "events.jsonl"));
const ws = "w1";
const sid = core.repos.session.create({ workspaceId: ws, title: "s", startedAt: Date.now() });

submitMessage({ core, workspaceId: ws, sessionId: sid, rawContent: "/define parry" });
submitMessage({ core, workspaceId: ws, sessionId: sid, rawContent: "/intends confidence" });
submitMessage({ core, workspaceId: ws, sessionId: sid, rawContent: "/refine timing window" });

const mech = (core.repos.mechanic.list(ws) as any[])[0];
assert.equal(mech.name, "parry");
assert.equal(mech.intends, "confidence");

core.close();
console.log("e2e-axis1-flow passed");
