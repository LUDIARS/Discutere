import fs from "node:fs";
import path from "node:path";
import assert from "node:assert/strict";
import { createCore } from "../../../src/core/index.js";
import { submitMessage } from "../../../src/core/projection/message-input.js";

const dir = path.resolve(".tmp/p2-synth");
fs.rmSync(dir, { recursive: true, force: true });
fs.mkdirSync(dir, { recursive: true });
const core = createCore(path.join(dir, "db.kuzu"), path.join(dir, "events.jsonl"));
const ws = "w1";
const sid = core.repos.session.create({ workspaceId: ws, title: "s", startedAt: Date.now() });

submitMessage({ core, workspaceId: ws, sessionId: sid, rawContent: "/propose Better onboarding" });
let r = submitMessage({ core, workspaceId: ws, sessionId: sid, rawContent: "/submit" });
assert.equal(r.commandResult?.ok, false);

const gapId = core.repos.designGap.create({ workspaceId: ws, title: "gap" });
r = submitMessage({ core, workspaceId: ws, sessionId: sid, rawContent: `/addresses ${gapId}` });
assert.equal(r.commandResult?.ok, true);
r = submitMessage({ core, workspaceId: ws, sessionId: sid, rawContent: "/submit" });
assert.equal(r.commandResult?.ok, true);

r = submitMessage({ core, workspaceId: ws, sessionId: sid, rawContent: "/integrate" });
assert.equal(r.commandResult?.ok, false);
r = submitMessage({ core, workspaceId: ws, sessionId: sid, rawContent: "/validate emotion" });
assert.equal(r.commandResult?.ok, true);
r = submitMessage({ core, workspaceId: ws, sessionId: sid, rawContent: "/integrate" });
assert.equal(r.commandResult?.ok, true);

core.close();
console.log("synthesis-handlers passed");
