/**
 * learning loop テスト (PR-F / MISSING #3-1 + #3-2 + #3-3).
 */

import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import Database from "better-sqlite3";

import {
  applyPersonaEngineMigrations,
  DISCUSSION_PERSONA_SEEDS,
  PersonasRepo,
  personaMetrics,
  recordHypothesisOutcome,
  ruleMetrics,
  RulesRepo,
} from "../../src/persona-engine/index.js";

const workDir = path.resolve(".tmp/pe-learning");
fs.rmSync(workDir, { recursive: true, force: true });
fs.mkdirSync(workDir, { recursive: true });
const db = new Database(path.join(workDir, "pe.db"));
applyPersonaEngineMigrations(db);
const personas = new PersonasRepo(db);
personas.bulkSeed(DISCUSSION_PERSONA_SEEDS);
const rules = new RulesRepo(db);

// 1. rule_log に "persona X proposed hypothesis Y" を仕込む
const hypIntegrated = "h-001";
const hypRejected = "h-002";
const hypPending = "h-003";
rules.log({
  rule_id: "propose-on-gap",
  action: "fire",
  actor: "ai",
  detail: `persona advocate proposed hypothesis ${hypIntegrated}`,
});
rules.log({
  rule_id: "propose-on-gap",
  action: "fire",
  actor: "ai",
  detail: `persona advocate proposed hypothesis ${hypRejected}`,
});
rules.log({
  rule_id: "propose-on-gap",
  action: "fire",
  actor: "ai",
  detail: `persona sceptic proposed hypothesis ${hypPending}`,
});

// 2. recordHypothesisOutcome で persona の learned_notes に記録
const r1 = recordHypothesisOutcome({
  personas,
  rules,
  hypothesisId: hypIntegrated,
  outcome: "integrated",
  statement: "選択肢を 2 つに絞る",
});
assert.equal(r1.recorded, true);
assert.equal(r1.personaId, "advocate");
console.log("ok recordHypothesisOutcome integrated");

const r2 = recordHypothesisOutcome({
  personas,
  rules,
  hypothesisId: hypRejected,
  outcome: "rejected",
});
assert.equal(r2.recorded, true);
assert.equal(r2.personaId, "advocate");
console.log("ok recordHypothesisOutcome rejected");

// 3. advocate の learned_notes に 2 件積まれる
const advocate = personas.get("advocate");
const notes = JSON.parse(advocate!.learned_notes);
assert.equal(notes.length, 2);
assert.ok(notes[0].note.includes("integrated") || notes[1].note.includes("integrated"));
assert.ok(notes[0].note.includes("rejected") || notes[1].note.includes("rejected"));
console.log("ok learned_notes appended");

// 4. unknown hypothesis (rule_log に提案者なし) → recorded: false
const ghost = recordHypothesisOutcome({
  personas,
  rules,
  hypothesisId: "h-ghost-id",
  outcome: "integrated",
});
assert.equal(ghost.recorded, false);
console.log("ok unknown hypothesis returns recorded: false");

// 5. personaMetrics: hypothesis status resolver を渡して採用率を集計
const resolver = (hid: string) => {
  if (hid === hypIntegrated) return "integrated" as const;
  if (hid === hypRejected) return "rejected" as const;
  return "pending" as const;
};
const metrics = personaMetrics(personas, rules, resolver);
const advocateMetric = metrics.find((m) => m.id === "advocate");
assert.ok(advocateMetric);
assert.equal(advocateMetric?.proposed, 2);
assert.equal(advocateMetric?.integrated, 1);
assert.equal(advocateMetric?.rejected, 1);
assert.equal(advocateMetric?.acceptanceRate, 0.5);
const scepticMetric = metrics.find((m) => m.id === "sceptic");
assert.ok(scepticMetric);
assert.equal(scepticMetric?.proposed, 1);
assert.equal(scepticMetric?.integrated, 0);
assert.equal(scepticMetric?.rejected, 0);
assert.equal(scepticMetric?.acceptanceRate, 0);
console.log("ok personaMetrics aggregates proposed/integrated/rejected");

// 6. 提案 0 件の persona は acceptanceRate=null
const refinerMetric = metrics.find((m) => m.id === "refiner");
assert.ok(refinerMetric);
assert.equal(refinerMetric?.proposed, 0);
assert.equal(refinerMetric?.acceptanceRate, null);
console.log("ok persona with 0 proposals → acceptanceRate null");

// 7. ruleMetrics: fires/skips/errors の集計
rules.log({ rule_id: "refute-cold", action: "skip", actor: "engine", detail: "cooldown" });
rules.log({ rule_id: "refute-cold", action: "skip", actor: "engine", detail: "cap" });
rules.log({ rule_id: "refute-cold", action: "error", actor: "ai", detail: "invalid JSON" });
const rmetrics = ruleMetrics(rules);
const proposeOnGap = rmetrics.find((m) => m.id === "propose-on-gap");
assert.ok(proposeOnGap);
assert.equal(proposeOnGap?.fires, 3); // step 1 の 3 件
const refuteCold = rmetrics.find((m) => m.id === "refute-cold");
assert.ok(refuteCold);
assert.equal(refuteCold?.skips, 2);
assert.equal(refuteCold?.errors, 1);
console.log("ok ruleMetrics aggregates fires/skips/errors");

db.close();
console.log("learning.test.ts: all passed");
