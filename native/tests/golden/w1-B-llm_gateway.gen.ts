// @ludiars/llm-gateway (Node 実装) の入出力を記録する。実行: npx tsx native/tests/golden/w1-B-llm_gateway.gen.ts
// 出力: native/tests/fixtures/llm_gateway/cases.json
import fs from "node:fs";
import path from "node:path";
import { estimateTokens, orderSegments, pickTier, type Segment } from "@ludiars/llm-gateway";

const texts = [
  "",
  "a",
  "abcd",
  "abcde",
  "日本語のテキスト",
  "english text here",
  "混在 mixed テキスト 123 ！？",
  "ｶﾀｶﾅ半角とＡＢＣ全角",
  "絵文字😀と𠮷野家",
  "぀ヿ㐀䶿一鿿豈﫿＀￯〿￰",
  "x".repeat(1001),
];
const tokens = texts.map((text) => ({ text, tokens: estimateTokens(text) }));

const segmentCases: Segment[][] = [
  [
    { stability: "volatile", role: "user", text: "history tail" },
    { stability: "fixed", role: "system", text: "persona" },
    { stability: "semi", role: "user", text: "attached doc" },
  ],
  [
    { stability: "fixed", role: "system", text: "rule A" },
    { stability: "fixed", role: "system", text: "rule B" },
    { stability: "semi", role: "system", text: "rag" },
    { stability: "volatile", role: "user", text: "now" },
    { stability: "volatile", role: "user", text: "again" },
    { stability: "fixed", role: "assistant", text: "ack" },
  ],
  [
    { stability: "fixed", role: "system", text: "  \n\t " },
    { stability: "volatile", role: "user", text: "hi" },
  ],
  [],
];
const ordering = segmentCases.map((segments) => ({ segments, result: orderSegments(segments) }));

const tierCases = [
  { input: { kind: "utterance" } },
  { input: { kind: "converge" } },
  { input: { kind: "utterance", forceTier: "strong" } },
  { input: { kind: "utterance", inputTokens: 8000 } },
  { input: { kind: "utterance", inputTokens: 8001 } },
  { input: { kind: "review" }, rules: { strongKinds: ["review"] } },
  { input: { kind: "converge" }, rules: { strongKinds: ["review"] } },
  { input: { kind: "utterance", inputTokens: 3000 }, rules: { strongAboveTokens: 2000 } },
];
const tiers = tierCases.map((c) => ({ ...c, tier: pickTier(c.input as never, c.rules as never) }));

const out = { tokens, ordering, tiers };
const target = path.resolve("native/tests/fixtures/llm_gateway/cases.json");
fs.writeFileSync(target, JSON.stringify(out, null, 2) + "\n");
console.log(`wrote ${target}`);
