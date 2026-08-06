import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import { Hono } from "hono";

const TMP_DIR = path.resolve(".tmp/flow-test/persona-routes");
fs.mkdirSync(TMP_DIR, { recursive: true });
const configFile = path.join(TMP_DIR, "discutere.config.json");
const databaseFile = path.join(TMP_DIR, "discutere.db");
for (const suffix of ["", "-shm", "-wal"]) fs.rmSync(`${databaseFile}${suffix}`, { force: true });
fs.writeFileSync(configFile, JSON.stringify({ flow: { autoAdoptOnCrawl: false } }, null, 2) + "\n", "utf8");

const previousConfig = process.env.DISCUTERE_CONFIG;
const previousDatabase = process.env.DATABASE_PATH;
const previousAutoAdopt = process.env.DISCUTERE_FLOW_AUTO_ADOPT_ON_CRAWL;
process.env.DISCUTERE_CONFIG = configFile;
process.env.DATABASE_PATH = databaseFile;
delete process.env.DISCUTERE_FLOW_AUTO_ADOPT_ON_CRAWL;

const { _resetConfig } = await import("../../src/config.js");
const { _resetFlowDb } = await import("../../src/flow/db/connection.js");
_resetConfig();
_resetFlowDb();

try {
  const { personaRoutes } = await import("../../src/api/persona-routes.js");
  const app = new Hono();
  app.route("/api", personaRoutes);

  const before = await app.request("/api/admin/personas/data");
  assert.equal(before.status, 200);
  assert.equal((await before.json()).autoAdoptOnCrawl, false);

  const saved = await app.request("/api/admin/personas/auto-adopt", {
    method: "PUT",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ enabled: true }),
  });
  assert.equal(saved.status, 200);
  assert.equal((await saved.json()).autoAdoptOnCrawl, true);

  const file = JSON.parse(fs.readFileSync(configFile, "utf8")) as {
    flow?: { autoAdoptOnCrawl?: boolean };
  };
  assert.equal(file.flow?.autoAdoptOnCrawl, true);
  console.log("  [ok] persona admin route: autoAdoptOnCrawl config save");

  const imported = await app.request("/api/admin/personas/import", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({
      personas: [{
        user_id: "ext:voluptas:abcdef0123456789",
        affect_vector: new Array(20).fill(0.1),
        vector_spec_version: 1,
        traits: ["協力型"],
      }],
    }),
  });
  assert.equal(imported.status, 200);
  assert.equal((await imported.json()).inserted, 1);

  const rawSid = await app.request("/api/admin/personas/import", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify([{ user_id: "raw-sid", affect_vector: new Array(20).fill(0), vector_spec_version: 1 }]),
  });
  assert.equal(rawSid.status, 200);
  assert.equal((await rawSid.json()).skipped, 1);
  console.log("  [ok] persona admin route: Voluptas pseudonymous import only");

  // NDJSON は壊れた行があっても取込全体を落とさず、件数だけ返す (CLI と同じ挙動)。
  const ndjson = await app.request("/api/admin/personas/import", {
    method: "POST",
    headers: { "content-type": "application/x-ndjson" },
    body: [
      JSON.stringify({
        pseudoId: "ext:voluptas:fedcba9876543210",
        affectVector: new Array(20).fill(0.2),
        vectorSpecVersion: 1,
      }),
      "not-json",
      "",
    ].join("\n"),
  });
  assert.equal(ndjson.status, 200);
  const ndjsonSummary = await ndjson.json() as {
    inserted: number;
    skipped: number;
    skipReasons: Record<string, number>;
  };
  assert.equal(ndjsonSummary.inserted, 1);
  assert.equal(ndjsonSummary.skipped, 1);
  assert.deepEqual(ndjsonSummary.skipReasons, { "invalid-json": 1 });
  console.log("  [ok] persona admin route: NDJSON upload degrades on broken lines");
} finally {
  if (previousConfig === undefined) delete process.env.DISCUTERE_CONFIG;
  else process.env.DISCUTERE_CONFIG = previousConfig;
  if (previousDatabase === undefined) delete process.env.DATABASE_PATH;
  else process.env.DATABASE_PATH = previousDatabase;
  if (previousAutoAdopt === undefined) delete process.env.DISCUTERE_FLOW_AUTO_ADOPT_ON_CRAWL;
  else process.env.DISCUTERE_FLOW_AUTO_ADOPT_ON_CRAWL = previousAutoAdopt;
  _resetConfig();
  _resetFlowDb();
}
