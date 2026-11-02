// persona-engine (Node 実装) のプロンプト組み立て・seed・単価推定の入出力を記録する。
// 実行: npx tsx native/tests/golden/w1-B-persona_engine.gen.ts
// 出力: native/tests/fixtures/persona_engine/*.json
import fs from "node:fs";
import path from "node:path";

import { buildPrompt } from "../../../src/persona-engine/engine/prompt-builder.js";
import type { DiscussionContextProvider } from "../../../src/persona-engine/context-provider.js";
import {
  buildAufhebungJudgePrompt,
  buildConvergePrompt,
  buildExpandPrompt,
} from "../../../src/persona-engine/facilitator/prompts.js";
import { DISCUSSION_PERSONA_SEEDS } from "../../../src/persona-engine/seeds/personas.js";
import { DISCUSSION_RULE_SEEDS, SEED_RULE_IDS } from "../../../src/persona-engine/seeds/rules.js";
import { DEBATE_RULE_SEEDS, RULE_INSTRUCTION_DEFAULTS } from "../../../src/persona-engine/worker-pool/debate-rules.js";
import {
  DEFAULT_WORKERS,
  ROLE_GUIDANCE_DEFAULTS,
  buildStandingPrompt,
  buildWorkerPersonaSeeds,
} from "../../../src/persona-engine/worker-pool/persona-prompts.js";
import { estimateCostUsd, modelFamily } from "../../../src/persona-engine/llm/pricing.js";
import { resolveTierModel } from "../../../src/persona-engine/llm/tier-routing.js";
import type { PersonaRow, RuleRow } from "../../../src/persona-engine/types.js";

const outDir = path.resolve("native/tests/fixtures/persona_engine");
const write = (name: string, value: unknown) => {
  fs.writeFileSync(path.join(outDir, name), JSON.stringify(value, null, 2) + "\n");
  console.log(`wrote ${name}`);
};

// ── seeds ──
write("seeds.json", {
  personas: DISCUSSION_PERSONA_SEEDS,
  rules: DISCUSSION_RULE_SEEDS,
  seedRuleIds: [...SEED_RULE_IDS],
  debateRules: DEBATE_RULE_SEEDS,
  ruleInstructionDefaults: RULE_INSTRUCTION_DEFAULTS,
  defaultWorkers: DEFAULT_WORKERS,
  roleGuidanceDefaults: ROLE_GUIDANCE_DEFAULTS,
  workerPersonaSeeds: buildWorkerPersonaSeeds([...DEFAULT_WORKERS, { id: "x-new", role: "謎役", provider: "codex", model: "gpt-x" }]),
  standingPrompts: [...DEFAULT_WORKERS, { id: "x-new", role: "謎役", provider: "codex" as const, model: "gpt-x" }].map((w) => ({
    worker: w,
    prompt: buildStandingPrompt({ worker: w }),
  })),
});

// ── facilitator prompts ──
const recent = [
  { speaker: "押野 進", content: "まず仮説を出そう" },
  { speaker: "参加者", content: "ガチャの天井は?\n二行目" },
];
write("facilitator.json", {
  expand: [
    { topic: "議題A", existing: ["推進派", "懐疑者"], recent, out: buildExpandPrompt({ topic: "議題A", existingPersonaNames: ["推進派", "懐疑者"], recent }) },
    { topic: "", existing: [], recent: [], out: buildExpandPrompt({ topic: "", existingPersonaNames: [], recent: [] }) },
    { topic: "t", existing: [""], recent: [], out: buildExpandPrompt({ topic: "t", existingPersonaNames: [""], recent: [] }) },
  ],
  aufhebung: [
    { topic: "議題B", recent, stocked: ["止揚1", "止揚2"], out: buildAufhebungJudgePrompt({ topic: "議題B", recent, stockedSummaries: ["止揚1", "止揚2"] }) },
    { topic: "議題B", recent: [], stocked: [], out: buildAufhebungJudgePrompt({ topic: "議題B", recent: [], stockedSummaries: [] }) },
  ],
  converge: [
    { topic: "議題C", recent, auf: ["a1"], top: ["高評価 / 人間", "x"], out: buildConvergePrompt({ topic: "議題C", recent, aufhebungen: ["a1"], topOpinions: ["高評価 / 人間", "x"] }) },
    { topic: "議題C", recent: [], out: buildConvergePrompt({ topic: "議題C", recent: [] }) },
    { topic: "議題C", recent, auf: [], top: [], out: buildConvergePrompt({ topic: "議題C", recent, aufhebungen: [], topOpinions: [] }) },
  ],
});

// ── prompt-builder ──
const persona: PersonaRow = {
  id: "sceptic", name: "懐疑者", display_name: "問田 鋭", description: "弱点を探す役",
  traits: JSON.stringify(["疑り深い", 3, "鋭い"]), speech_style: "鋭く", skill_template: "", learned_notes: "[]",
  created_at: 1, updated_at: 2,
};
const rule: RuleRow = {
  id: "refute-cold", description: "説明", trigger_type: "tick", tick_sec: 300, event_kind: null, conditions: "[]",
  instructions: "指示1\n\n指示2", target: "sceptic", cooldown_sec: 180, last_fired_at: null, enabled: 1, added_at: 1,
  added_by: "seed", removed_at: null, removed_by: null, removed_reason: null,
};
type Scenario = {
  name: string; sessionId: string | null; gaps: unknown[]; voices: boolean; directives: string[]; active: string | null;
  rule?: Partial<RuleRow>; persona?: Partial<PersonaRow>;
};
const gapsFull = [
  { id: "g-closed", title: "閉じた議題", description: "古い", status: "closed", gapIn: null, expectedAffect: null, observedAffect: null, hypothesisIds: [], updatedAt: 5 },
  { id: "g1", title: "  ガチャの天井、 是か非か!? ", description: "対象ゲーム: モンスト「天井」/ 確率・課金 (2026)—検討", status: "OPEN", gapIn: "mech", expectedAffect: "joy", observedAffect: null, hypothesisIds: ["h1", "h2"], updatedAt: 4 },
];
const scenarios: Scenario[] = [
  { name: "full", sessionId: "s1", gaps: gapsFull, voices: true, directives: ["もっと簡単な言葉で", "否定的に"], active: null },
  { name: "tick-active-session", sessionId: null, gaps: gapsFull, voices: false, directives: [], active: "s-active" },
  { name: "tick-no-session", sessionId: null, gaps: [], voices: true, directives: ["x"], active: null },
  { name: "all-closed", sessionId: "s1", gaps: [gapsFull[0]], voices: true, directives: [], active: null,
    rule: { description: null }, persona: { traits: "not json" } },
  { name: "null-description", sessionId: "", gaps: [{ ...gapsFull[1], description: null, title: "a" }], voices: true, directives: [], active: "zz",
    persona: { traits: "{\"a\":1}" } },
];
const promptCases = scenarios.map((s) => {
  const calls: unknown[] = [];
  const provider: DiscussionContextProvider = {
    listActiveHypotheses: (ws, limit) => { calls.push(["hyp", ws, limit]); return [{ id: "h1", statement: "天井は必要", status: "proposed", designGapId: "g1", updatedAt: 10 }, { id: "h2", statement: "不要", status: "proposed", designGapId: null, updatedAt: 9 }]; },
    listRecentGaps: (ws, limit) => { calls.push(["gaps", ws, limit]); return s.gaps as never; },
    listRecentUtterances: (ws, sid, limit) => { calls.push(["utt", ws, sid, limit]); return [{ id: "u1", sessionId: sid, rawContent: "人間です \"引用\"", postedAt: 100 }]; },
    listRelevantExternalVoices: (ws, terms, limit) => { calls.push(["voices", ws, terms, limit]); return s.voices ? [{ id: "v1", content: "天井ありがたい", source: "youtube", sourceUrl: "https://example.com/v", speakerLabel: "論者#a1b2", score: 0.75 }, { id: "v2", content: "x", source: "steam", sourceUrl: null, speakerLabel: "外部の声", score: 1 }] : []; },
    listFacilitatorDirectives: (ws, gapId, limit) => { calls.push(["dir", ws, gapId, limit]); return s.directives; },
    proposeHypothesis: () => ({ id: "x" }),
    postUtterance: () => ({ id: "x" }),
    findActiveDiscussionSession: (input) => { calls.push(["active", input.workspaceId]); return s.active; },
  };
  const out = buildPrompt({
    rule: { ...rule, ...(s.rule ?? {}) } as RuleRow, persona: { ...persona, ...(s.persona ?? {}) } as PersonaRow,
    workspaceId: "ws", sessionId: s.sessionId, contextProvider: provider, triggeredBy: "tick",
  });
  return { scenario: s, system: out.system, user: out.user, calls };
});
write("prompt_builder.json", promptCases);

// ── pricing / tier ──
const usages = [
  undefined,
  {},
  { input_tokens: 1000, output_tokens: 500 },
  { input_tokens: 1234, output_tokens: 567, cache_read_input_tokens: 8901, cache_creation_input_tokens: 2345 },
  { input_tokens: 0, output_tokens: 0 },
];
const models = [undefined, "", "claude-opus-4-8", "CLAUDE-SONNET-5@high", "claude-haiku-4-5-20251001", "gpt-5.6-sol", "x@opus", "opus"];
write("pricing.json", {
  families: models.map((m) => ({ model: m ?? null, family: modelFamily(m) })),
  costs: models.flatMap((m) => usages.map((u) => ({ model: m ?? null, usage: u ?? null, cost: estimateCostUsd(m, u) ?? null }))),
  tiers: [
    { input: { kind: "converge" }, tiers: null },
    { input: { kind: "converge" }, tiers: {} },
    { input: { kind: "converge" }, tiers: { cheap: "c", strong: "s" } },
    { input: { kind: "utterance" }, tiers: { cheap: "c", strong: "s" } },
    { input: { kind: "converge" }, tiers: { cheap: "c" } },
    { input: { kind: "utterance" }, tiers: { strong: "s" } },
    { input: { kind: "converge" }, tiers: { cheap: "c", strong: "" } },
    { input: { kind: "utterance", inputTokens: 9000 }, tiers: { cheap: "c", strong: "s" } },
  ].map((c) => ({ ...c, model: resolveTierModel(c.input, (c.tiers ?? undefined) as never) ?? null })),
});
