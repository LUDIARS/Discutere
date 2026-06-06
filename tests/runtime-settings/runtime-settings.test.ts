import assert from "node:assert/strict";
import Database from "better-sqlite3";

import {
  createRuntimeSettingsStore,
  mergeFacilitatorTuning,
  type FacilitatorTuning,
} from "../../src/runtime-settings/store.js";
import { applyRuleInstructionOverrides } from "../../src/persona-engine/worker-pool/debate-rules.js";
import type { RuleSeed } from "../../src/persona-engine/types.js";

const DEF: FacilitatorTuning = {
  tickMs: 30_000,
  idleGapMs: 120_000,
  maxPersonas: 20,
  aufhebungTarget: 3,
  convergePolicy: "default",
};

// ── mergeFacilitatorTuning: 部分 override + クランプ + policy 検証 ──
assert.deepEqual(mergeFacilitatorTuning(DEF, null), DEF);
assert.equal(mergeFacilitatorTuning(DEF, { maxPersonas: 8 }).maxPersonas, 8);
assert.equal(mergeFacilitatorTuning(DEF, { maxPersonas: 0 }).maxPersonas, 1); // 下限クランプ
assert.equal(mergeFacilitatorTuning(DEF, { maxPersonas: NaN }).maxPersonas, 20); // 不正→default
assert.equal(
  mergeFacilitatorTuning(DEF, { convergePolicy: "aufhebung-only" }).convergePolicy,
  "aufhebung-only",
);
assert.equal(
  mergeFacilitatorTuning(DEF, { convergePolicy: "bogus" as never }).convergePolicy,
  "default",
); // enum 外→default
console.log("ok mergeFacilitatorTuning");

// ── store: facilitator override の永続 + reload ──
{
  const db = new Database(":memory:");
  const store = createRuntimeSettingsStore(db, {
    facilitator: DEF,
    rolePrompts: { ファシリテーター: "司会の既定", 意見屋: "意見屋の既定" },
    ruleInstructions: { "tick-con-opus": "否定の既定" },
  });

  assert.deepEqual(store.getFacilitatorTuning(), DEF); // override 無し → default
  const eff = store.setFacilitatorTuning({ maxPersonas: 12, convergePolicy: "aufhebung-only" });
  assert.equal(eff.maxPersonas, 12);
  assert.equal(eff.convergePolicy, "aufhebung-only");
  assert.equal(store.getFacilitatorTuning().idleGapMs, DEF.idleGapMs); // 他は default 維持

  // 別 store で同じ DB を開いて永続を確認
  const store2 = createRuntimeSettingsStore(db, {
    facilitator: DEF,
    rolePrompts: {},
    ruleInstructions: {},
  });
  assert.equal(store2.getFacilitatorTuning().maxPersonas, 12);
  console.log("ok store facilitator persist");

  // ── role prompt override / reset ──
  assert.equal(store.getRolePrompt("ファシリテーター"), "司会の既定");
  store.setRolePrompt("ファシリテーター", "新しい司会指示");
  assert.equal(store.getRolePrompt("ファシリテーター"), "新しい司会指示");
  const facView = store.listRolePrompts().find((v) => v.key.endsWith("ファシリテーター"))!;
  assert.equal(facView.override, "新しい司会指示");
  assert.equal(facView.default, "司会の既定");
  store.setRolePrompt("ファシリテーター", ""); // 空 → リセット
  assert.equal(store.getRolePrompt("ファシリテーター"), "司会の既定");
  console.log("ok store role prompt override/reset");

  // ── rule instruction override ──
  assert.equal(store.getRuleInstruction("tick-con-opus"), "否定の既定");
  store.setRuleInstruction("tick-con-opus", "もっと鋭く反論して");
  assert.equal(store.getRuleInstruction("tick-con-opus"), "もっと鋭く反論して");
  console.log("ok store rule instruction override");
}

// ── applyRuleInstructionOverrides: override のみ差し替え、 他は保持 ──
{
  const seeds: RuleSeed[] = [
    { id: "a", description: "", trigger_type: "tick", target: "x", cooldown_sec: 10, instructions: "元A" },
    { id: "b", description: "", trigger_type: "tick", target: "y", cooldown_sec: 10, instructions: "元B" },
  ];
  const out = applyRuleInstructionOverrides(seeds, (id) => (id === "a" ? "新A" : undefined));
  assert.equal(out[0].instructions, "新A"); // override 適用
  assert.equal(out[1].instructions, "元B"); // override 無し → 保持
  assert.equal(seeds[0].instructions, "元A"); // 元配列は不変 (pure)
  // 空文字 override は無視 (= 既定保持)
  const out2 = applyRuleInstructionOverrides(seeds, () => "  ");
  assert.equal(out2[0].instructions, "元A");
  console.log("ok applyRuleInstructionOverrides");
}

console.log("runtime-settings tests: all passed");
