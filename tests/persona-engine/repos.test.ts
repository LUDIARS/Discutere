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
import { DEBATE_RULE_SEEDS } from "../../src/persona-engine/worker-pool/debate-rules.js";

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
assert.equal(propose?.event_kind, "DesignGapDetected");
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

// 8. DEBATE_RULE_SEEDS の tick rule は必ず tick_sec を持つ (回帰防止)。
//    tick_sec が無いと engine.ts の `if (!r.tick_sec) continue` で永久 skip され、
//    自走 debate が一切始まらない (= 「議論が始まらない」 不具合の根因)。
for (const seed of DEBATE_RULE_SEEDS) {
  if (seed.trigger_type === "tick") {
    assert.ok(
      typeof seed.tick_sec === "number" && seed.tick_sec > 0,
      `debate tick rule ${seed.id} must have positive tick_sec (got ${seed.tick_sec})`
    );
  }
}
console.log("ok debate tick rules carry tick_sec");

// 9. reconcileSeed は既存行の発火パラメータを seed に追従させる (self-heal)。
//    INSERT OR IGNORE では既存 tick_sec=null 行が更新されないので reconcile で直す。
const stale = { id: "reconcile-target", trigger_type: "tick" as const, target: "advocate", instructions: "x" };
rules.insertOrIgnore(stale); // tick_sec 無し → null で入る
assert.equal(rules.get("reconcile-target")?.tick_sec, null);
rules.reconcileSeed({ ...stale, tick_sec: 90, cooldown_sec: 90 });
const healed = rules.get("reconcile-target");
assert.equal(healed?.tick_sec, 90);
assert.equal(healed?.cooldown_sec, 90);
console.log("ok reconcileSeed heals tick_sec");

db.close();
console.log("repos.test.ts: all passed");
