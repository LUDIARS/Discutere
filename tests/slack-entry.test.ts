/**
 * Slack 入口の純粋関数テスト (node:assert/strict、tsx で実行)。
 *
 * 本リポの既存テストは vitest ではなく node:assert + tsx 実行 (`tsx tests/.../run.ts`)
 * の形式なので、それに合わせる。単体実行: `npx tsx tests/slack-entry.test.ts`。
 */

import assert from "node:assert/strict";

import {
  SLACK_FLOW_PICK_ACTION,
  SLACK_FLOW_SETTINGS_ACTION,
  buildFlowPickBlocks,
  buildFlowSettingsBlocks,
  firstActionId,
  firstSelectedValue,
  isThreadStarter,
  parseSettingsPreset,
} from "../src/slack/slack-entry.js";
import { parseFlowKind } from "../src/flow/dispatch.js";

// ── parseSettingsPreset (Discord 版と同仕様) ──
assert.deepEqual(parseSettingsPreset("default"), {});
assert.deepEqual(parseSettingsPreset("bogus"), {});
assert.deepEqual(parseSettingsPreset("2x4"), { rounds: 2, turnsPerRound: 4 });
assert.deepEqual(parseSettingsPreset("8x12"), { rounds: 8, turnsPerRound: 12 });
console.log("ok parseSettingsPreset");

// ── isThreadStarter ──
// トップレベル投稿 (thread_ts 無し) → true。
assert.equal(isThreadStarter({ type: "message", ts: "100.1", text: "テーマ" }), true);
// thread_ts === ts (スレッド親) → true。
assert.equal(isThreadStarter({ type: "message", ts: "100.1", thread_ts: "100.1" }), true);
// スレッド返信 (thread_ts !== ts) → false。
assert.equal(isThreadStarter({ type: "message", ts: "200.2", thread_ts: "100.1" }), false);
// subtype 付き (編集/参加通知等) → false。
assert.equal(isThreadStarter({ type: "message", ts: "100.1", subtype: "message_changed" }), false);
// bot 投稿 → false。
assert.equal(isThreadStarter({ type: "message", ts: "100.1", bot_id: "B1" }), false);
// 非 message / null → false。
assert.equal(isThreadStarter({ type: "reaction_added", ts: "100.1" }), false);
assert.equal(isThreadStarter(null), false);
assert.equal(isThreadStarter({ type: "message" }), false); // ts 無し
console.log("ok isThreadStarter");

// ── buildFlowPickBlocks の構造 ──
{
  const blocks = buildFlowPickBlocks();
  assert.ok(Array.isArray(blocks) && blocks.length >= 2, "pick blocks は section + actions");
  const actions = blocks.find((b) => (b as { type?: string }).type === "actions") as
    | { elements: Array<Record<string, unknown>> }
    | undefined;
  assert.ok(actions, "actions ブロックがある");
  const select = actions!.elements[0] as { type: string; action_id: string; options: Array<{ value: string }> };
  assert.equal(select.type, "static_select");
  assert.equal(select.action_id, SLACK_FLOW_PICK_ACTION);
  // value は parseFlowKind が解決できる日本語ラベルであること (フロー起動の前提)。
  const values = select.options.map((o) => o.value);
  assert.deepEqual(values, ["議論", "改善", "学習", "壁打ち"]);
  for (const v of values) {
    assert.ok(parseFlowKind(v), `pick option "${v}" が FlowKind に解決できない`);
  }
}
console.log("ok buildFlowPickBlocks");

// ── buildFlowSettingsBlocks の構造 ──
{
  const blocks = buildFlowSettingsBlocks();
  const actions = blocks.find((b) => (b as { type?: string }).type === "actions") as
    | { elements: Array<Record<string, unknown>> }
    | undefined;
  assert.ok(actions, "actions ブロックがある");
  const select = actions!.elements[0] as { type: string; action_id: string; options: Array<{ value: string }> };
  assert.equal(select.type, "static_select");
  assert.equal(select.action_id, SLACK_FLOW_SETTINGS_ACTION);
  const values = select.options.map((o) => o.value);
  assert.deepEqual(values, ["default", "2x4", "3x6", "5x8", "8x12"]);
  // "default" 以外は parseSettingsPreset が rounds/turns に解決できる。
  for (const v of values.filter((x) => x !== "default")) {
    const parsed = parseSettingsPreset(v);
    assert.ok(parsed.rounds && parsed.turnsPerRound, `settings option "${v}" が解決できない`);
  }
}
console.log("ok buildFlowSettingsBlocks");

// ── firstActionId / firstSelectedValue (block_actions payload からの抽出) ──
{
  const payload = {
    type: "block_actions",
    actions: [{ action_id: SLACK_FLOW_PICK_ACTION, selected_option: { value: "改善" } }],
  };
  assert.equal(firstActionId(payload), SLACK_FLOW_PICK_ACTION);
  assert.equal(firstSelectedValue(payload), "改善");
  // 空 actions → undefined。
  assert.equal(firstActionId({ actions: [] }), undefined);
  assert.equal(firstSelectedValue({ actions: [] }), undefined);
}
console.log("ok payload extraction");

console.log("\nall slack-entry tests passed");
