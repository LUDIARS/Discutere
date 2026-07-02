/**
 * ペーパー出所ラベル (08-paper-provenance) のテスト。
 *   - curated/crawl と llm 増補が md 上で節分離される (出所併記 / 仮説メカニクス節)
 *   - round-trip (md ⇄ 構造化) で source が保存される
 *   - 旧 mechanics (source なし) が curated として読める (後方互換)
 *   - buildPaperSystem (旧経路) にも節構造が載る / 増補 0 件なら仮説節を出さない
 */

import assert from "node:assert/strict";

import {
  paperDraftToMarkdown,
  markdownToPaperDraft,
  groundedMechanics,
  hypotheticalMechanics,
  hypothesisSectionMarkdown,
  HEAD_HYPOTHESIS_MECHANICS,
  type PaperContent,
} from "../../src/flow/paper-markdown.js";
import { buildPersonaPaper, buildPaperSystem, type DiscussionPaper } from "../../src/flow/discussion-paper.js";
import { enrichMechanics } from "../../src/flow/mechanic-extract.js";
import { MockLLMClient } from "../../src/persona-engine/llm/mock.js";
import type { FlowPersona } from "../../src/flow/personas.js";
import type { MechanicSummary } from "../../src/flow/investigate.js";

const mixedMechanics: MechanicSummary[] = [
  { name: "引っ張り", description: "敵に当てる", intended_affect: "爽快", source: "curated" },
  { name: "周回", description: "素材集め", source: "crawl" },
  { name: "天井", description: "一定回数で確定入手", intended_affect: "安心", source: "llm" },
];

// ── md 節分離: curated/crawl は出所併記、llm は仮説節へ ──────────────────────
{
  const content: PaperContent = { theme: "T", tags: [], supplement: "", mechanics: mixedMechanics };
  const md = paperDraftToMarkdown(content);
  assert.ok(md.includes("# ゲームのメカニクス"), "grounded 節がある");
  assert.ok(md.includes("- **引っ張り**: 敵に当てる → 期待感情: 爽快 （出所: curated）"), "curated 出所併記");
  assert.ok(md.includes("- **周回**: 素材集め （出所: crawl）"), "crawl 出所併記");
  assert.ok(md.includes(HEAD_HYPOTHESIS_MECHANICS), "仮説節がある");
  // 仮説節の中に llm 分が入り、grounded 節には入らない
  const hypIdx = md.indexOf(HEAD_HYPOTHESIS_MECHANICS);
  assert.ok(md.indexOf("- **天井**") > hypIdx, "llm 分は仮説節側");
  assert.ok(!md.slice(0, hypIdx).includes("天井"), "grounded 節に llm 分が混ざらない");
}
console.log("ok provenance md 節分離 (出所併記 + 仮説節)");

// ── round-trip で source が保存される ────────────────────────────────────────
{
  const content: PaperContent = { theme: "T", tags: [], supplement: "S", mechanics: mixedMechanics };
  const md = paperDraftToMarkdown(content);
  const back = markdownToPaperDraft(md, { theme: "FB", tags: [], supplement: "", mechanics: [] });
  assert.equal(back.mechanics.length, 3);
  const byName = new Map(back.mechanics.map((m) => [m.name, m]));
  assert.equal(byName.get("引っ張り")?.source, "curated");
  assert.equal(byName.get("引っ張り")?.intended_affect, "爽快", "出所併記があっても期待感情を保持");
  assert.equal(byName.get("周回")?.source, "crawl");
  assert.equal(byName.get("天井")?.source, "llm", "仮説節の項目は source=llm で復元");
  assert.equal(byName.get("天井")?.intended_affect, "安心");
}
console.log("ok provenance round-trip source 保存");

// ── 旧データ (source なし) は curated 扱いで round-trip ──────────────────────
{
  const legacy: PaperContent = {
    theme: "T",
    tags: [],
    supplement: "",
    mechanics: [{ name: "落下", description: "果物を落とす" }],
  };
  const md = paperDraftToMarkdown(legacy);
  assert.ok(md.includes("- **落下**: 果物を落とす （出所: curated）"), "source なしは curated 扱い");
  assert.ok(!md.includes(HEAD_HYPOTHESIS_MECHANICS), "旧データだけなら仮説節は出ない");
  const back = markdownToPaperDraft(md, legacy);
  assert.equal(back.mechanics[0].source, "curated");
}
console.log("ok provenance 旧データ後方互換 (curated 扱い)");

// ── 出所併記のない旧 md も読める (後方互換) ──────────────────────────────────
{
  const md = "# 議題\nT\n\n# ゲームのメカニクス\n- **落下**: 果物を落とす → 期待感情: 爽快";
  const back = markdownToPaperDraft(md, { theme: "T", tags: [], supplement: "", mechanics: [] });
  assert.equal(back.mechanics.length, 1);
  assert.equal(back.mechanics[0].name, "落下");
  assert.equal(back.mechanics[0].intended_affect, "爽快");
  assert.equal(back.mechanics[0].source, undefined, "出所併記なし = undefined (curated 扱い)");
}
console.log("ok provenance 出所併記なしの旧 md 読込");

// ── buildPaperSystem (旧経路 = bodyMd なし) にも節構造が載る ─────────────────
{
  const paper: DiscussionPaper = {
    paperId: "p1",
    sessionId: "s1",
    theme: "T",
    tags: [],
    supplement: "",
    mechanics: mixedMechanics,
    rounds: [],
  };
  const persona: FlowPersona = {
    id: "per1",
    name: "太郎",
    role: "debater",
    traits: ["冷静"],
    speechStyle: "です・ます",
    model: "mock",
    isLocal: false,
  };
  const pp = buildPersonaPaper({
    paper,
    persona,
    stance: "pro",
    currentRoundUtterances: [],
    userVoices: [],
  });
  const system = buildPaperSystem(pp);
  assert.ok(system.includes(HEAD_HYPOTHESIS_MECHANICS), "system に仮説節が載る");
  assert.ok(system.includes("（出所: curated）"), "system に出所併記が載る");
}
console.log("ok provenance buildPaperSystem 節構造");

// ── 増補 0 件なら仮説節自体が出ない ──────────────────────────────────────────
{
  const onlyGrounded: MechanicSummary[] = [{ name: "A", description: "d", source: "curated" }];
  assert.equal(hypothesisSectionMarkdown(onlyGrounded), null);
  const md = paperDraftToMarkdown({ theme: "T", tags: [], supplement: "", mechanics: onlyGrounded });
  assert.ok(!md.includes(HEAD_HYPOTHESIS_MECHANICS), "空の仮説節を作らない");
}
console.log("ok provenance 増補 0 件で仮説節なし");

// ── enrichMechanics の増補分は source=llm が付く ─────────────────────────────
{
  const llm = new MockLLMClient([
    () => JSON.stringify([{ name: "新規", description: "感想由来", intended_affect: "期待" }]),
  ]);
  const out = await enrichMechanics({
    theme: "T",
    existing: [{ name: "既存", description: "d", source: "curated" }],
    voices: [{ content: "楽しい", source: "niconico" }],
    llm,
    target: 5,
  });
  assert.equal(out.find((m) => m.name === "既存")?.source, "curated", "既存の source は不変");
  assert.equal(out.find((m) => m.name === "新規")?.source, "llm", "増補分は source=llm");
  assert.equal(groundedMechanics(out).length, 1);
  assert.equal(hypotheticalMechanics(out).length, 1);
}
console.log("ok provenance enrichMechanics source=llm");

// ── 固定フォーム md でも仮説節が分離され round-trip する ─────────────────────
{
  const fixed: PaperContent = {
    theme: "T",
    tags: [],
    supplement: "",
    mechanics: mixedMechanics,
    gameTitle: "ゲームX",
    discussionTheme: "テンポ改善",
    discussionContent: "判断したい",
    mechanicsContext: "基本ループの説明",
    themeSupplement: "補足",
  };
  const md = paperDraftToMarkdown(fixed);
  assert.ok(md.includes(HEAD_HYPOTHESIS_MECHANICS), "固定フォーム md にも仮説節");
  const back = markdownToPaperDraft(md, fixed);
  const byName = new Map(back.mechanics.map((m) => [m.name, m]));
  assert.equal(byName.get("天井")?.source, "llm");
  assert.equal(byName.get("引っ張り")?.source, "curated");
  assert.equal(back.mechanicsContext, "基本ループの説明", "mechanicsContext に仮説節が混ざらない");
}
console.log("ok provenance 固定フォーム round-trip");

console.log("paper-provenance tests: all ok");
