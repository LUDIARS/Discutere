import fs from "node:fs";
import path from "node:path";
import assert from "node:assert/strict";
import { createCore } from "../../../../src/core/index.js";
import { createManualGap } from "../../../../src/core/bridge/gap/manual.js";

const dir = path.resolve(".tmp/p4-manual");
fs.rmSync(dir, { recursive: true, force: true });
fs.mkdirSync(dir, { recursive: true });
const core = createCore(path.join(dir, "db.kuzu"), path.join(dir, "events.jsonl"));
const ws = "w1";

assert.throws(() => createManualGap(core, { workspaceId: ws, mechanicId: "m1", expectedAffect: "joy", observedAffect: "anger", evidenceUtteranceIds: [] }));
const id = createManualGap(core, { workspaceId: ws, mechanicId: "m1", expectedAffect: "joy", observedAffect: "anger", evidenceUtteranceIds: ["u1"] });
assert.ok(id);

core.close();
console.log("gap manual passed");
