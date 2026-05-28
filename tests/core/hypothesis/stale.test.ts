import fs from "node:fs";
import path from "node:path";
import assert from "node:assert/strict";
import { createCore } from "../../../src/core/index.js";
import { runStaleDetectionJob } from "../../../src/core/jobs/stale-detection-job.js";

const dir = path.resolve(".tmp/p5-stale");
fs.rmSync(dir, { recursive: true, force: true });
fs.mkdirSync(dir, { recursive: true });
const core = createCore(path.join(dir, "db.kuzu"), path.join(dir, "events.jsonl"));
const ws = "w1";
const hid = core.repos.hypothesis.create({ workspaceId: ws, statement: "old", status: "proposed" } as any);
core.client.raw.prepare("UPDATE hypotheses SET updated_at = ? WHERE id = ?").run(Date.now() - 31 * 24 * 60 * 60 * 1000, hid);
const flagged = runStaleDetectionJob(core, ws, Date.now());
assert.equal(flagged.length, 1);
const h = core.repos.hypothesis.get(hid) as any;
assert.equal(h.stale_flagged, 1);
core.close();
console.log("hypothesis stale passed");
