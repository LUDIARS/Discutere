/**
 * persona-engine の safety caps (PR-A).
 *
 * - maxFiresPerSession (turn budget)
 * - maxFiresPerRulePerSession (無限ループ防止)
 * - resetSession で cap が解放される
 * - sessionId なし (tick rule) は cap 対象外
 */

import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import Database from "better-sqlite3";

import {
  applyPersonaEngineMigrations,
  createPersonaEngine,
  DISCUSSION_PERSONA_SEEDS,
  DISCUSSION_RULE_SEEDS,
  MockLLMClient,
  type DiscussionContextProvider,
  type Logger,
} from "../../src/persona-engine/index.js";

const workDir = path.resolve(".tmp/pe-safety");
fs.rmSync(workDir, { recursive: true, force: true });
fs.mkdirSync(workDir, { recursive: true });
const db = new Database(path.join(workDir, "pe.db"));
applyPersonaEngineMigrations(db);

const proposed: Array<{ statement: string; persona: string }> = [];
const ctx: DiscussionContextProvider = {
  listActiveHypotheses: () => [],
  listRecentGaps: () => [
    {
      id: "g-1",
      title: "g",
      status: "open",
      gapIn: null,
      expectedAffect: null,
      observedAffect: null,
      hypothesisIds: [],
      updatedAt: 1,
    },
  ],
  listRecentUtterances: () => [],
  proposeHypothesis: (input) => {
    proposed.push({ statement: input.statement, persona: input.byPersonaId });
    return { id: `h-${proposed.length}` };
  },
  postUtterance: (input) => ({ id: `u-${proposed.length}` }),
};

const logger: Logger = {
  debug: () => {},
  info: () => {},
  warn: () => {},
  error: () => {},
};

// MockLLM: 常に propose_hypothesis を返す (= 無限に提案したがる)
const llm = new MockLLMClient(
  Array.from({ length: 10 }, (_, i) => () =>
    `{"action":"propose_hypothesis","statement":"S${i}","addresses_gap_id":"g-1","reasoning":"r"}`
  )
);

const sessionA = "session-A";
const sessionB = "session-B";

const engine = createPersonaEngine({
  db,
  llm,
  contextProvider: ctx,
  logger,
  workspaceId: "knowledge",
  personaSeeds: DISCUSSION_PERSONA_SEEDS,
  ruleSeeds: DISCUSSION_RULE_SEEDS,
  tickMs: 60_000,
  enableLlm: true,
  maxFiresPerSession: 2,
  maxFiresPerRulePerSession: 1,
});

// 1. session A の propose-on-gap を 1 回叩く → 通る (rule cap=1)
db.prepare("UPDATE rules SET last_fired_at = NULL, cooldown_sec = 0").run();
await engine.fireRule("propose-on-gap", "manual", sessionA);
assert.equal(proposed.length, 1, "first fire ok");

// 2. 同 session 同 rule の再 fire → "rule cap reached" でスキップ
db.prepare("UPDATE rules SET last_fired_at = NULL").run();
await engine.fireRule("propose-on-gap", "manual-2", sessionA);
assert.equal(proposed.length, 1, "rule cap blocked second fire");
const lastLog = db
  .prepare("SELECT detail FROM rule_log ORDER BY id DESC LIMIT 1")
  .get() as { detail: string };
assert.ok(
  lastLog.detail.includes("rule cap reached"),
  `expected rule cap log, got: ${lastLog.detail}`
);
console.log("ok per-rule cap blocks loop");

// 3. 別 rule (refute-cold) を session A で 1 回叩く → 通る (rule cap は per-rule)
// (MockLLM は常に propose を返すため、 refute-cold でも proposed が +1 される — それで OK)
db.prepare("UPDATE rules SET last_fired_at = NULL").run();
await engine.fireRule("refute-cold", "manual", sessionA);
assert.equal(proposed.length, 2, "refute-cold fired under per-rule cap (LLM returned propose anyway)");
// session A の total fires は 2 (propose 1 + refute 1) = maxFiresPerSession=2 上限
console.log("ok per-rule cap is independent per rule");

// 4. session A 上限到達 → 3 回目 (別の rule でも) はブロック
db.prepare("UPDATE rules SET last_fired_at = NULL").run();
await engine.fireRule("integrate-on-many", "manual", sessionA);
assert.equal(proposed.length, 2, "session cap should block third fire");
const log2 = db
  .prepare("SELECT detail FROM rule_log ORDER BY id DESC LIMIT 1")
  .get() as { detail: string };
assert.ok(
  log2.detail.includes("session cap reached"),
  `expected session cap log, got: ${log2.detail}`
);
console.log("ok per-session total cap blocks");

// 5. session B は独立カウンタ → 通る
db.prepare("UPDATE rules SET last_fired_at = NULL").run();
await engine.fireRule("propose-on-gap", "manual", sessionB);
assert.equal(proposed.length, 3, "session B independent (proposed += 1)");
console.log("ok session counter is per-session");

// 6. resetSession(A) → A も再び発火可能
engine.resetSession(sessionA);
db.prepare("UPDATE rules SET last_fired_at = NULL").run();
await engine.fireRule("propose-on-gap", "manual", sessionA);
assert.equal(proposed.length, 4, "after reset, session A can fire again");
console.log("ok resetSession releases cap");

// 7. sessionId null (tick rule 想定) は cap 対象外
db.prepare("UPDATE rules SET last_fired_at = NULL").run();
const initialProposed = proposed.length;
for (let i = 0; i < 5; i += 1) {
  db.prepare("UPDATE rules SET last_fired_at = NULL").run();
  await engine.fireRule("propose-on-gap", "tick", null);
}
assert.ok(
  proposed.length - initialProposed >= 5,
  "tick rules (sessionId=null) should bypass session cap"
);
console.log("ok null sessionId bypasses cap (tick rules)");

engine.stop();
db.close();
console.log("safety-caps.test.ts: all passed");
