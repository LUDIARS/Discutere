import fs from "node:fs";
import path from "node:path";
import assert from "node:assert/strict";
import { createCore } from "../../../src/core/index.js";
import { submitMessage } from "../../../src/core/projection/message-input.js";

const dir = path.resolve(".tmp/p2-axis3-submit");
fs.rmSync(dir, { recursive: true, force: true });
fs.mkdirSync(dir, { recursive: true });
const core = createCore(path.join(dir, "db.kuzu"), path.join(dir, "events.jsonl"));
const ws = "w1";
const sid = core.repos.session.create({ workspaceId: ws, title: "s", startedAt: Date.now() });

submitMessage({ core, workspaceId: ws, sessionId: sid, rawContent: "/propose title" });
const r = submitMessage({ core, workspaceId: ws, sessionId: sid, rawContent: "/submit" });
assert.equal(r.commandResult?.ok, false);
assert.match(r.commandResult?.error ?? "", /addresses/i);

core.close();
console.log("e2e-axis3-submit-guard passed");
