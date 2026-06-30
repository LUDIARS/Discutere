/**
 * Anatomia ドメイン → メカニクス精製 (refine.ts) + メカニクス結合 (paper-review.mergeMechanics) のテスト。
 *   - 決定論フォールバック: 実装メカニクススラグを持つドメインだけ採用。
 *   - LLM 精製: JSON 配列を MechanicSummary に正規化 / 失敗・空は フォールバックに degrade。
 *   - mergeMechanics: 名前で重複排除 (先勝ち)。
 */

import assert from "node:assert/strict";

import {
  refineDomainsToMechanics,
  fallbackDomainsToMechanics,
} from "../../src/flow/anatomia/refine.js";
import type { AnatomiaDomainCard } from "../../src/flow/anatomia/types.js";
import { mergeMechanics } from "../../src/flow/paper-review.js";
import { MockLLMClient } from "../../src/persona-engine/llm/mock.js";

const CARDS: AnatomiaDomainCard[] = [
  { name: "Trial & Judgment", description: "裁判", mechanics: ["trial", "death-or-education"], specRefs: [] },
  { name: "Client UI (PixiJS)", description: "描画", mechanics: [], specRefs: [] }, // infra/表示層 (スラグ無し)
  { name: "Incident Lifecycle", description: "事件", mechanics: ["incident"], specRefs: [], rationale: "境界" },
];

// ── 決定論フォールバック: スラグありだけ採用 ─────────────────────────────────
{
  const out = fallbackDomainsToMechanics(CARDS);
  assert.equal(out.length, 2, "スラグ無しの Client UI は落ちる");
  assert.equal(out[0].name, "Trial & Judgment");
  assert.ok(out[0].description.includes("実装メカニクス: trial, death-or-education"));
  assert.equal(out[0].intended_affect, undefined, "フォールバックは intended_affect を付けない");
}

// ── llm 未指定なら フォールバック ─────────────────────────────────────────────
{
  const out = await refineDomainsToMechanics(CARDS);
  assert.equal(out.length, 2, "llm 未指定 → 決定論フォールバック");
}

// ── 空入力は [] ──────────────────────────────────────────────────────────────
{
  const out = await refineDomainsToMechanics([], { llm: new MockLLMClient([]) });
  assert.deepEqual(out, []);
}

// ── LLM 精製: JSON 配列を正規化 (player-facing + intended_affect) ─────────────
{
  const llm = new MockLLMClient([
    () =>
      JSON.stringify([
        { name: "裁判", description: "村人を糾弾して死刑か教育を選ぶ", intended_affect: "緊張と裁定の重み" },
        { name: "事件", description: "村で事件が起きる", intended_affect: "波乱への期待" },
      ]),
  ]);
  const out = await refineDomainsToMechanics(CARDS, { llm, theme: "Pagus の面白さ" });
  assert.equal(out.length, 2);
  assert.equal(out[0].name, "裁判");
  assert.equal(out[0].intended_affect, "緊張と裁定の重み");
  assert.ok(llm.lastInvocation?.prompt.includes("Pagus の面白さ"), "theme が prompt に乗る");
}

// ── LLM 失敗 → フォールバック ────────────────────────────────────────────────
{
  const llm = new MockLLMClient([() => ({ ok: false, error: "boom" })]);
  const out = await refineDomainsToMechanics(CARDS, { llm });
  assert.equal(out.length, 2, "LLM 失敗時は決定論フォールバック");
  assert.equal(out[0].name, "Trial & Judgment");
}

// ── LLM 応答が JSON でない → フォールバック ──────────────────────────────────
{
  const llm = new MockLLMClient([() => "解析できませんでした"]);
  const out = await refineDomainsToMechanics(CARDS, { llm });
  assert.equal(out.length, 2, "JSON 抽出失敗時は決定論フォールバック");
}

// ── mergeMechanics: 名前で重複排除 (先勝ち = Anatomia 優先) ──────────────────
{
  const anatomia = [{ name: "裁判", description: "Anatomia 由来" }];
  const investigate = [
    { name: "裁判", description: "investigate 由来 (落ちる)" },
    { name: "経済", description: "investigate 由来" },
  ];
  const merged = mergeMechanics(anatomia, investigate);
  assert.equal(merged.length, 2);
  assert.equal(merged[0].name, "裁判");
  assert.equal(merged[0].description, "Anatomia 由来", "先勝ちで Anatomia を優先");
  assert.equal(merged[1].name, "経済");
}

console.log("anatomia-refine tests: passed");
