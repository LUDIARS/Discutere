import fs from "node:fs";
import path from "node:path";
import assert from "node:assert/strict";
import { createCore } from "../../../src/core/index.js";
import { runStaleDetectionJob } from "../../../src/core/jobs/stale-detection-job.js";

const dir = path.resolve(".tmp/p5-e2e-stale");
fs.rmSync(dir, { recursive: true, force: true });
fs.mkdirSync(dir, { recursive: true });
const core = createCore(path.join(dir, "db.kuzu"), path.join(dir, "events.jsonl"));
const ws = "w1";
const h1 = core.repos.hypothesis.create({ workspaceId: ws, statement: "stale", status: "proposed" } as any);
const h2 = core.repos.hypothesis.create({ workspaceId: ws, statement: "done", status: "integrated", integrated: true } as any);
core.client.raw.prepare("UPDATE hypotheses SET updated_at = ? WHERE id IN (?, ?)").run(Date.now() - 31 * 24 * 60 * 60 * 1000, h1, h2);
const flagged = runStaleDetectionJob(core, ws, Date.now());
assert.equal(flagged.includes(h1), true);
assert.equal(flagged.includes(h2), false);
core.close();
console.log("hypothesis e2e stale passed");
