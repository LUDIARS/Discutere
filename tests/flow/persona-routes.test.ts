import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import { Hono } from "hono";

const TMP_DIR = path.resolve(".tmp/flow-test/persona-routes");
fs.mkdirSync(TMP_DIR, { recursive: true });
const configFile = path.join(TMP_DIR, "discutere.config.json");
fs.writeFileSync(configFile, JSON.stringify({ flow: { autoAdoptOnCrawl: false } }, null, 2) + "\n", "utf8");

const previousConfig = process.env.DISCUTERE_CONFIG;
const previousAutoAdopt = process.env.DISCUTERE_FLOW_AUTO_ADOPT_ON_CRAWL;
process.env.DISCUTERE_CONFIG = configFile;
delete process.env.DISCUTERE_FLOW_AUTO_ADOPT_ON_CRAWL;

const { _resetConfig } = await import("../../src/config.js");
_resetConfig();

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
} finally {
  if (previousConfig === undefined) delete process.env.DISCUTERE_CONFIG;
  else process.env.DISCUTERE_CONFIG = previousConfig;
  if (previousAutoAdopt === undefined) delete process.env.DISCUTERE_FLOW_AUTO_ADOPT_ON_CRAWL;
  else process.env.DISCUTERE_FLOW_AUTO_ADOPT_ON_CRAWL = previousAutoAdopt;
  _resetConfig();
}
