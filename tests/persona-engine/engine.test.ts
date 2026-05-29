/**
 * Mock LLM で engine の fire 経路を end-to-end 検証。
 *
 * - propose_hypothesis action が ContextProvider に届く
 * - skip / unknown action / 不正 JSON は rule_log に error / skip
 * - cooldown 中の rule 再呼出は no-op
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

const workDir = path.resolve(".tmp/pe-engine");
fs.rmSync(workDir, { recursive: true, force: true });
fs.mkdirSync(workDir, { recursive: true });
const db = new Database(path.join(workDir, "pe.db"));
applyPersonaEngineMigrations(db);

// in-memory ContextProvider
let proposed: Array<{ statement: string; gapId: string | null; persona: string }> = [];
let posted: Array<{ text: string; persona: string; sessionId: string }> = [];
const ctx: DiscussionContextProvider = {
  listActiveHypotheses: () => [
    { id: "h-stale", statement: "stale hypothesis", status: "proposed", designGapId: "g-1", updatedAt: 1 },
  ],
  listRecentGaps: () => [
    {
      id: "g-1",
      title: "gap test",
      status: "open",
      gapIn: null,
      expectedAffect: null,
      observedAffect: null,
      hypothesisIds: ["h-stale"],
      updatedAt: 2,
    },
  ],
  listRecentUtterances: () => [],
  proposeHypothesis: (input) => {
    proposed.push({
      statement: input.statement,
      gapId: input.designGapId ?? null,
      persona: input.byPersonaId,
    });
    return { id: `h-${proposed.length}` };
  },
  postUtterance: (input) => {
    posted.push({
      text: input.text,
      persona: input.byPersonaId,
      sessionId: input.sessionId,
    });
    return { id: `u-${posted.length}` };
  },
};

const collectedLogs: Array<{ level: string; meta: Record<string, unknown>; msg: string }> = [];
const logger: Logger = {
  debug: () => {},
  info: (m, msg) => collectedLogs.push({ level: "info", meta: m, msg }),
  warn: (m, msg) => collectedLogs.push({ level: "warn", meta: m, msg }),
  error: (m, msg) => collectedLogs.push({ level: "error", meta: m, msg }),
};

// MockLLM: 1 回目 propose_hypothesis (gap_id 付き), 2 回目 不正 JSON, 3 回目 skip
const llm = new MockLLMClient([
  () => '{"action":"propose_hypothesis","statement":"絞ったオンボーディング","addresses_gap_id":"g-1","reasoning":"test"}',
  () => "not a json {{",
  () => '{"action":"skip","reasoning":"nothing to say"}',
]);

const engine = createPersonaEngine({
  db,
  llm,
  contextProvider: ctx,
  logger,
  workspaceId: "knowledge",
  personaSeeds: DISCUSSION_PERSONA_SEEDS,
  ruleSeeds: DISCUSSION_RULE_SEEDS,
  // tick は使わず fireRule で手動駆動
  tickMs: 60_000,
  enableLlm: true,
});

// 1. fireRule で propose-on-gap → proposed に積まれる
await engine.fireRule("propose-on-gap", "manual");
assert.equal(proposed.length, 1, "first fire should propose hypothesis");
assert.equal(proposed[0].statement, "絞ったオンボーディング");
assert.equal(proposed[0].gapId, "g-1");
assert.equal(proposed[0].persona, "advocate");
console.log("ok first fire (propose_hypothesis)");

// 2. cooldown 中なので 2 回目の即時 fire は no-op (cooldown 60s)
await engine.fireRule("propose-on-gap", "manual-retry");
assert.equal(proposed.length, 1, "cooldown should block second fire");
assert.equal(llm.calls, 1, "llm should not be called during cooldown");
console.log("ok cooldown blocks immediate re-fire");

// 3. 別 rule (refute-cold tick) を手動駆動 → 不正 JSON → error ログ
await engine.fireRule("refute-cold", "manual");
const errLogs = collectedLogs.filter((l) => l.level === "warn");
assert.ok(
  errLogs.some((l) => l.msg.includes("invalid JSON")),
  "invalid JSON should produce warn log"
);
console.log("ok invalid JSON handled");

// 4. integrate-on-many → skip action
await engine.fireRule("integrate-on-many", "manual");
assert.equal(posted.length, 0, "skip should not post utterance");
assert.equal(proposed.length, 1, "skip should not propose");
console.log("ok skip action");

// 5. fireEvent: propose-on-gap は cooldown 中なのでスキップ、 但しイベント発火経路自体は動く
await engine.fireEvent({ kind: "GapDetected" });
console.log("ok fireEvent path (cooldown still blocking)");

// 6. setRulesEnabled(false) で全 rule 停止
engine.setRulesEnabled(false);
// cooldown を強制解除して fire
db.prepare("UPDATE rules SET last_fired_at = NULL").run();
await engine.fireRule("refute-cold", "manual");
const lastSkip = db
  .prepare("SELECT detail FROM rule_log ORDER BY id DESC LIMIT 1")
  .get() as { detail: string };
assert.ok(
  lastSkip.detail.includes("runtime"),
  `last log should be runtime kill switch skip, got: ${lastSkip.detail}`
);
console.log("ok runtime kill switch");

engine.stop();
db.close();
console.log("engine.test.ts: all passed");
