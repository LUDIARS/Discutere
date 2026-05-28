import fs from "node:fs";
import path from "node:path";
import assert from "node:assert/strict";
import { createCore } from "../../../src/core/index.js";
import { submitMessage } from "../../../src/core/projection/message-input.js";

const dir = path.resolve(".tmp/p5-e2e");
fs.rmSync(dir, { recursive: true, force: true });
fs.mkdirSync(dir, { recursive: true });
const core = createCore(path.join(dir, "db.kuzu"), path.join(dir, "events.jsonl"));
const ws = "w1";
const sid = core.repos.session.create({ workspaceId: ws, title: "s", startedAt: Date.now(), mode: "emotion" } as any);

submitMessage({ core, workspaceId: ws, sessionId: sid, rawContent: "/propose h1" });
const gapId = core.repos.designGap.create({ workspaceId: ws, title: "gap" });
submitMessage({ core, workspaceId: ws, sessionId: sid, rawContent: `/addresses ${gapId}` });
assert.equal(submitMessage({ core, workspaceId: ws, sessionId: sid, rawContent: "/submit" }).commandResult?.ok, true);
assert.equal(submitMessage({ core, workspaceId: ws, sessionId: sid, rawContent: "/validate theory" }).commandResult?.ok, true);
assert.equal(submitMessage({ core, workspaceId: ws, sessionId: sid, rawContent: "/done" }).commandResult?.ok, true);
assert.equal(submitMessage({ core, workspaceId: ws, sessionId: sid, rawContent: "/validate emotion" }).commandResult?.ok, true);
assert.equal(submitMessage({ core, workspaceId: ws, sessionId: sid, rawContent: "/done" }).commandResult?.ok, true);
assert.equal(submitMessage({ core, workspaceId: ws, sessionId: sid, rawContent: "/integrate" }).commandResult?.ok, true);

const h = core.client.raw.prepare("SELECT status FROM hypotheses ORDER BY updated_at DESC LIMIT 1").get() as any;
assert.equal(h.status, "integrated");
core.close();
console.log("hypothesis e2e lifecycle passed");
