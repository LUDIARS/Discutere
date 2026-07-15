import assert from "node:assert/strict";
import test from "node:test";

import { GlabLaunchStore } from "../src/integrations/glab-launch.js";

test("GLab launch tickets are one-time and resolve a Cernere actor session", () => {
  let now = 1_000;
  let sequence = 0;
  const store = new GlabLaunchStore(() => now, () => `id-${++sequence}`);
  const ticket = store.createLaunch("cernere-user-1");
  const launch = store.consumeLaunch(ticket);
  assert.deepEqual(launch, { actorSession: "id-2", cernereUserId: "cernere-user-1" });
  assert.equal(store.consumeLaunch(ticket), null);
  assert.equal(store.resolveActor("id-2"), "cernere-user-1");

  now += 8 * 60 * 60 * 1000;
  assert.equal(store.resolveActor("id-2"), null);
});

test("expired GLab launch tickets cannot be consumed", () => {
  let now = 1_000;
  const store = new GlabLaunchStore(() => now, () => "ticket");
  const ticket = store.createLaunch("cernere-user-2");
  now += 60_000;
  assert.equal(store.consumeLaunch(ticket), null);
});
