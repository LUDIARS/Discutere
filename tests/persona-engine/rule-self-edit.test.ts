/**
 * rule 自己補正テスト (PR-J).
 *
 * - add_rule: schema 検証 / forbidden patterns / id 衝突 / 正常追加
 * - remove_rule: 不存在 / seed 保護 / 正常削除
 * - admin GET /admin/rules フィルタ
 */

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
  SEED_RULE_IDS,
} from "../../src/persona-engine/index.js";
import {
  __test as handlerTest,
  handleAction,
} from "../../src/persona-engine/engine/handler.js";

void handlerTest; // suppress unused

const workDir = path.resolve(".tmp/pe-rule-self-edit");
fs.rmSync(workDir, { recursive: true, force: true });
fs.mkdirSync(workDir, { recursive: true });
const db = new Database(path.join(workDir, "pe.db"));
applyPersonaEngineMigrations(db);
const personas = new PersonasRepo(db);
personas.bulkSeed(DISCUSSION_PERSONA_SEEDS);
const rules = new RulesRepo(db);
rules.bulkSeed(DISCUSSION_RULE_SEEDS);

const logger = { debug: () => {}, info: () => {}, warn: () => {}, error: () => {} };
const ctx = {
  listActiveHypotheses: () => [],
  listRecentGaps: () => [],
  listRecentUtterances: () => [],
  proposeHypothesis: () => ({ id: "x" }),
  postUtterance: () => ({ id: "y" }),
};

function runAction(text: string, ruleId = "meta-improve-rules", personaId = "neutral-observer") {
  return handleAction({
    ruleId,
    personaId,
    workspaceId: "knowledge",
    sessionId: null,
    rawText: text,
    contextProvider: ctx,
    rules,
    logger,
  });
}

// ─── add_rule ────────────────────────────────────

{
  // 1. 正常追加
  const r = runAction(JSON.stringify({
    action: "add_rule",
    rule: {
      id: "extra-counter",
      description: "テスト追加 rule",
      trigger_type: "tick",
      tick_sec: 600,
      target: "advocate",
      cooldown_sec: 300,
      instructions: "テスト用に追加された rule の指示文 — 16 文字以上必要なのでこの文章で長さを稼ぐ",
    },
    reasoning: "test",
  }));
  assert.equal(r.kind, "add_rule");
  assert.equal(r.createdId, "extra-counter");
  const stored = rules.get("extra-counter");
  assert.ok(stored);
  assert.equal(stored?.added_by, "ai:neutral-observer");
  console.log("ok add_rule success");
}

{
  // 2. id 衝突 (seed と被る)
  const r = runAction(JSON.stringify({
    action: "add_rule",
    rule: {
      id: "propose-on-gap",
      trigger_type: "tick",
      tick_sec: 100,
      instructions: "duplicate id should be rejected (16 文字以上にする)",
    },
  }));
  assert.equal(r.kind, "error");
  assert.match(r.detail, /already exists/);
  console.log("ok add_rule rejects duplicate id");
}

{
  // 3. forbidden pattern
  const r = runAction(JSON.stringify({
    action: "add_rule",
    rule: {
      id: "silent-check",
      trigger_type: "tick",
      tick_sec: 300,
      instructions: "沈黙確認用の rule、 これは禁則パターンに一致するため reject されるはず",
    },
  }));
  assert.equal(r.kind, "error");
  assert.match(r.detail, /forbidden/);
  console.log("ok add_rule rejects forbidden pattern");
}

{
  // 4. id 形式不正
  const r = runAction(JSON.stringify({
    action: "add_rule",
    rule: {
      id: "Invalid_Id_With_Caps",
      trigger_type: "tick",
      tick_sec: 100,
      instructions: "abcdefghijklmnopqrstuvwxyz (length over 16)",
    },
  }));
  assert.equal(r.kind, "error");
  assert.match(r.detail, /id invalid/);
  console.log("ok add_rule rejects bad id");
}

{
  // 5. instructions 短すぎ
  const r = runAction(JSON.stringify({
    action: "add_rule",
    rule: {
      id: "too-short-instr",
      trigger_type: "tick",
      tick_sec: 100,
      instructions: "short",
    },
  }));
  assert.equal(r.kind, "error");
  assert.match(r.detail, /instructions length/);
  console.log("ok add_rule rejects short instructions");
}

// ─── remove_rule ─────────────────────────────────

{
  // 6. seed 保護
  const r = runAction(JSON.stringify({
    action: "remove_rule",
    rule_id: "propose-on-gap",
    reasoning: "test remove",
  }));
  assert.equal(r.kind, "error");
  assert.match(r.detail, /protected seed/);
  // まだ存在
  assert.equal(rules.get("propose-on-gap")?.enabled, 1);
  console.log("ok remove_rule rejects seed");
}

{
  // 7. 不存在
  const r = runAction(JSON.stringify({
    action: "remove_rule",
    rule_id: "no-such-rule",
    reasoning: "test",
  }));
  assert.equal(r.kind, "error");
  assert.match(r.detail, /unknown id/);
  console.log("ok remove_rule rejects unknown id");
}

{
  // 8. 正常削除 (AI 追加の rule を削除)
  const r = runAction(JSON.stringify({
    action: "remove_rule",
    rule_id: "extra-counter",
    reasoning: "no longer needed",
  }));
  assert.equal(r.kind, "remove_rule");
  const after = rules.get("extra-counter");
  assert.equal(after?.enabled, 0);
  assert.equal(after?.removed_by, "ai:neutral-observer");
  assert.equal(after?.removed_reason, "no longer needed");
  console.log("ok remove_rule success");
}

// ─── SEED_RULE_IDS 公開確認 ───────────────────

assert.ok(SEED_RULE_IDS.has("propose-on-gap"));
assert.ok(SEED_RULE_IDS.has("meta-improve-rules"));
assert.equal(SEED_RULE_IDS.has("extra-counter"), false);
console.log("ok SEED_RULE_IDS exposes seed names");

db.close();
console.log("rule-self-edit.test.ts: all passed");
