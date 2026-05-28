import fs from "node:fs";
import path from "node:path";
import assert from "node:assert/strict";
import { createCore } from "../../../src/core/index.js";
import { applyLifecycle } from "../../../src/core/hypothesis/lifecycle-handler.js";

const dir = path.resolve(".tmp/p5-lifecycle");
fs.rmSync(dir, { recursive: true, force: true });
fs.mkdirSync(dir, { recursive: true });
const core = createCore(path.join(dir, "db.kuzu"), path.join(dir, "events.jsonl"));
const ws = "w1";
const hid = core.repos.hypothesis.create({ workspaceId: ws, statement: "h1", status: "draft" } as any);

let r = applyLifecycle(core, hid, "submit", { workspaceId: ws, sessionId: "s1" });
assert.equal(r.ok, true);
r = applyLifecycle(core, hid, "validate_theory", { workspaceId: ws, sessionId: "s1" });
assert.equal(r.ok, true);
r = applyLifecycle(core, hid, "integrate", { workspaceId: ws, sessionId: "s1" });
assert.equal(r.ok, false);

core.close();
console.log("hypothesis lifecycle passed");
