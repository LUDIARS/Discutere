import fs from "node:fs";
import path from "node:path";
import assert from "node:assert/strict";
import { createCore } from "../../../../src/core/index.js";
import { submitMessage } from "../../../../src/core/projection/message-input.js";

const dir = path.resolve(".tmp/p6-hotspot");
fs.rmSync(dir, { recursive: true, force: true });
fs.mkdirSync(dir, { recursive: true });
const core = createCore(path.join(dir, "db.kuzu"), path.join(dir, "events.jsonl"));
const ws = "w1";
const sid = core.repos.session.create({ workspaceId: ws, title: "s", startedAt: Date.now() } as any);
core.repos.mechanic.create({ workspaceId: ws, name: "dash" } as any);
core.repos.utterance.create({ workspaceId: ws, sessionId: sid, rawContent: "dash dash", postedAt: Date.now() } as any);
const r = submitMessage({ core, workspaceId: ws, sessionId: sid, rawContent: "/hotspot" });
assert.equal(r.commandResult?.ok, true);
assert.match(r.commandResult?.message ?? "", /dash/i);
core.close();
console.log("cross hotspot passed");
