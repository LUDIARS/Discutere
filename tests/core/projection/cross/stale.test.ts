import fs from "node:fs";
import path from "node:path";
import assert from "node:assert/strict";
import { createCore } from "../../../../src/core/index.js";
import { submitMessage } from "../../../../src/core/projection/message-input.js";

const dir = path.resolve(".tmp/p6-stale");
fs.rmSync(dir, { recursive: true, force: true });
fs.mkdirSync(dir, { recursive: true });
const core = createCore(path.join(dir, "db.kuzu"), path.join(dir, "events.jsonl"));
const ws = "w1";
const sid = core.repos.session.create({ workspaceId: ws, title: "s", startedAt: Date.now() } as any);
const hid = core.repos.hypothesis.create({ workspaceId: ws, statement: "stale", status: "proposed" } as any);
core.client.raw.prepare("UPDATE hypotheses SET stale_flagged = 1 WHERE id = ?").run(hid);
const r = submitMessage({ core, workspaceId: ws, sessionId: sid, rawContent: "/stale" });
assert.equal(r.commandResult?.ok, true);
assert.match(r.commandResult?.message ?? "", /stale/i);
core.close();
console.log("cross stale passed");
