import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import Database from "better-sqlite3";

import {
  applyPersonaEngineMigrations,
  DISCUSSION_PERSONA_SEEDS,
  DISCUSSION_RULE_SEEDS,
  PersonasRepo,
  RulesRepo,
} from "../../src/persona-engine/index.js";

const workDir = path.resolve(".tmp/pe-repos");
fs.rmSync(workDir, { recursive: true, force: true });
fs.mkdirSync(workDir, { recursive: true });
const db = new Database(path.join(workDir, "pe.db"));
applyPersonaEngineMigrations(db);

const personas = new PersonasRepo(db);
const rules = new RulesRepo(db);

// 1. migration が冪等
applyPersonaEngineMigrations(db);
console.log("ok migrations idempotent");

// 2. persona seed bulk insert
personas.bulkSeed(DISCUSSION_PERSONA_SEEDS);
const list = personas.list();
assert.equal(list.length, DISCUSSION_PERSONA_SEEDS.length);
const advocate = personas.get("advocate");
assert.ok(advocate);
assert.equal(advocate?.name, "推進派");
assert.equal(advocate?.display_name, "押野 進");
const traits = JSON.parse(advocate!.traits);
assert.ok(Array.isArray(traits) && traits.length > 0);
console.log("ok persona seed");

// 3. assignment lifecycle
const a = personas.assign("advocate", "session-1");
assert.equal(a.persona_id, "advocate");
const active = personas.activeAssignmentsForSession("session-1");
assert.equal(active.length, 1);
personas.release(a.id);
assert.equal(personas.activeAssignmentsForSession("session-1").length, 0);
console.log("ok assignment lifecycle");

// 4. learned_notes append
personas.appendLearnedNote("advocate", "first lesson");
const updated = personas.get("advocate");
const notes = JSON.parse(updated!.learned_notes);
assert.ok(Array.isArray(notes) && notes.length === 1);
assert.equal(notes[0].note, "first lesson");
console.log("ok learned_notes append");

// 5. rule seed
rules.bulkSeed(DISCUSSION_RULE_SEEDS);
const tickRules = rules.list({ enabled: true, trigger_type: "tick" });
const eventRules = rules.list({ enabled: true, trigger_type: "event" });
assert.ok(tickRules.length >= 1);
assert.ok(eventRules.length >= 1);
const propose = rules.get("propose-on-gap");
assert.ok(propose);
assert.equal(propose?.event_kind, "GapDetected");
assert.equal(propose?.target, "advocate");
console.log("ok rule seed");

// 6. log + setLastFired
rules.log({ rule_id: "propose-on-gap", action: "fire", actor: "engine", detail: "test" });
const logs = rules.recentLogs(10);
assert.equal(logs.length, 1);
assert.equal(logs[0].action, "fire");

rules.setLastFired("propose-on-gap", Math.floor(Date.now() / 1000));
const after = rules.get("propose-on-gap");
assert.ok(after?.last_fired_at);
console.log("ok rule log + setLastFired");

// 7. setEnabled + remove
rules.setEnabled("propose-on-gap", false);
const off = rules.list({ enabled: false });
assert.ok(off.some((r) => r.id === "propose-on-gap"));
rules.setEnabled("propose-on-gap", true);

rules.remove("propose-on-gap", "test", "trial");
const removed = rules.get("propose-on-gap");
assert.equal(removed?.enabled, 0);
assert.equal(removed?.removed_reason, "trial");
console.log("ok rule enable/remove");

db.close();
console.log("repos.test.ts: all passed");
