import assert from "node:assert/strict";
import Database from "better-sqlite3";
import { applyFlowMigrations } from "../../src/flow/db/migrations.js";
import { publicationState, publishedConclusions, setPublication } from "../../src/integrations/conclusion-publication.js";
import { upsertConclusionCache } from "../../src/visualize/conclusion-cache.js";
import { createConclusionPublicationRoutes } from "../../src/api/conclusion-publication-routes.js";

const db = new Database(":memory:");
try {
  applyFlowMigrations(db);
  applyFlowMigrations(db);
  for (const id of ["public", "glab", "private", "unset", "revoked"]) {
    upsertConclusionCache(db, { gapId: id, kind: "gap", sessionId: id, title: id, conclusion: "approved result", utteranceCount: 1, aufhebungCount: 0, updatedAt: 1 });
  }
  assert.deepEqual(publishedConclusions(db), []);
  for (const scope of ["public", "glab", "private"] as const) {
    assert.equal(setPublication(db, scope, true, scope, publicationState(db, scope)!.revision, 2), true);
  }
  assert.deepEqual(publishedConclusions(db).map(row => row.id).sort(), ["glab", "public"]);
  const revision = publicationState(db, "public")!.revision;
  db.prepare("UPDATE conclusion_cache SET conclusion='changed' WHERE gap_id='public'").run();
  assert.deepEqual(publishedConclusions(db, "public"), []);
  assert.equal(setPublication(db, "public", true, "public", revision, 3), false);
  assert.equal(setPublication(db, "glab", false, "glab", publicationState(db, "glab")!.revision, 3), true);
  assert.deepEqual(publishedConclusions(db), []);
  const secret = "a".repeat(40);
  const app = createConclusionPublicationRoutes(() => db, () => secret);
  assert.equal((await app.request("http://localhost/integrations/glab/conclusions")).status, 401);
  assert.equal((await app.request("http://localhost/integrations/glab/conclusions?id=private", { headers: { authorization: `Bearer ${secret}` } })).status, 404);
  assert.equal((await app.request("http://localhost/admin/conclusion-publication?id=public", { headers: { "X-User-Role": "admin" } })).status, 403);
  const env = { incoming: { socket: { remoteAddress: "127.0.0.1" } } };
  assert.equal((await app.request("http://public.example/admin/conclusion-publication?id=public", {}, env)).status, 403);
  assert.equal((await app.request("http://localhost/admin/conclusion-publication", { method: "PUT", headers: { origin: "http://evil.example", "content-type": "application/json" }, body: "{}" }, env)).status, 403);
  const state = publicationState(db, "public")!;
  assert.equal((await app.request("http://localhost/admin/conclusion-publication", { method: "PUT", headers: { origin: "http://localhost", "content-type": "application/json" }, body: JSON.stringify({ id: "public", revision: state.revision, scope: "public", publishable: true }) }, env)).status, 200);
  const response = await app.request("http://localhost/integrations/glab/conclusions", { headers: { authorization: `Bearer ${secret}` } });
  assert.equal(response.headers.get("cache-control"), "private, no-store");
  const body = await response.json();
  assert.deepEqual(Object.keys(body.conclusions[0]).sort(), ["conclusion", "id", "scope", "title", "updatedAt"]);
} finally {
  db.close();
}
