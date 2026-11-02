// src/external-discussion/{contracts,participation}.ts の入出力を記録する。
// 実行: npx tsx native/tests/golden/w1-A-external_discussion.gen.ts
import fs from "node:fs";
import path from "node:path";
import { parseConversation } from "../../../src/external-discussion/contracts.js";
import { opportunity } from "../../../src/external-discussion/participation.js";

const NOW = 10_000_000;
const msg = (id: unknown, kind: unknown, text: unknown, at: unknown) => ({ id, kind, text, at });
const inputs: unknown[] = [
  null, "x", { scene: "actio:a", enabled: true, messages: [] },
  { scene: "actio:", enabled: true, messages: [] },
  { scene: "actio:" + "a".repeat(220), enabled: true, messages: [] },
  { scene: "actio:" + "a".repeat(221), enabled: true, messages: [] },
  { scene: "actio:a b", enabled: true, messages: [] },
  { scene: "actio:a\n", enabled: true, messages: [] },
  { scene: "actio:a", enabled: "yes", messages: [] },
  { scene: "actio:a", enabled: true, messages: new Array(80).fill(0).map((_, i) => msg(`m${i}`, "human", "x", i)) },
  { scene: "actio:a", enabled: true, messages: new Array(81).fill(0).map((_, i) => msg(`m${i}`, "human", "x", i)) },
  { scene: "actio:a", enabled: true, messages: [msg("1", "human", "a", 1), msg("1", "human", "b", 2)] },
  { scene: "actio:a", enabled: true, messages: [msg("", "human", "a", 1)] },
  { scene: "actio:a", enabled: true, messages: [msg("x".repeat(200), "human", "a", 1)] },
  { scene: "actio:a", enabled: true, messages: [msg("x".repeat(201), "human", "a", 1)] },
  { scene: "actio:a", enabled: true, messages: [msg("1", "bot", "a", 1)] },
  { scene: "actio:a", enabled: true, messages: [msg("1", "human", "a", 1.5)] },
  { scene: "actio:a", enabled: true, messages: [msg("1", "human", "a", -1)] },
  { scene: "actio:a", enabled: true, messages: [msg("1", "human", "a", 2 ** 53)] },
  { scene: "actio:a", enabled: true, messages: [msg("1", "human", "a", "5")] },
  { scene: "actio:a", enabled: true, messages: [msg("1", "human", "あ".repeat(8000), 1)] },
  { scene: "actio:a", enabled: true, messages: [msg("1", "human", "あ".repeat(8001), 1)] },
  { scene: "actio:a", enabled: true, messages: [0, 1, 2, 3].map((i) => msg(`m${i}`, "human", "𝒳".repeat(4000), i)) },
  { scene: "actio:a", enabled: true, messages: [0, 1, 2, 3, 4].map((i) => msg(`m${i}`, "human", "x".repeat(6400), i)) },
  { scene: "actio:a", enabled: true, messages: [0, 1, 2, 3, 4].map((i) => msg(`m${i}`, "human", "x".repeat(6401), i)) },
  {
    scene: "actio:team:thread", enabled: false,
    messages: ["b", "B", "a", "A", "a-b", "ab", "a_b", "a1", "a10", "a2", "Ab", "aB", "_", "~", "9", "z"].map((id, i) => msg(id, i % 2 ? "ai" : "human", id, 5 - (i % 3))),
  },
];
const parse = inputs.map((input) => {
  try { return { input, ok: true, output: parseConversation(input) }; }
  catch (e) { return { input, ok: false, error: (e as Error).message }; }
});

const base = { scene: "actio:team:thread", enabled: true };
const opp = [
  { ...base, messages: [msg("1", "human", "どう進めよう", NOW - 179_999)] },
  { ...base, messages: [msg("1", "human", "どう進めよう", NOW - 180_000)] },
  { ...base, messages: [msg("1", "human", "どう進めよう", NOW - 180_000), msg("ai", "ai", "応答", NOW - 180_000)] },
  { ...base, messages: [msg("1", "human", "  ", NOW - 200_000)] },
  { ...base, messages: [msg("1", "human", "x", NOW + 1)] },
  { ...base, messages: [msg("1", "human", "x", NOW - 86_400_001)] },
  { ...base, messages: [msg("1", "human", "x", NOW - 86_400_000)] },
  { ...base, messages: [0, 1, 2].map((i) => msg(String(i), "human", "検討中", NOW - 1000 + i)) },
  { ...base, messages: [0, 1].map((i) => msg(String(i), "human", "検討中", NOW - 1000 + i)) },
  { ...base, enabled: false, messages: [msg("1", "human", "x", NOW - 200_000)] },
  { scene: "actio:日本語", enabled: true, messages: [msg("id\"q", "human", "改行\nと\"引用\"", NOW - 200_000)] },
];
const opportunities = opp.map((input) => ({ input, now: NOW, output: opportunity(input as never, NOW) }));

const dir = path.join(path.dirname(new URL(import.meta.url).pathname), "..", "fixtures", "external_discussion");
fs.writeFileSync(path.join(dir, "contracts.json"), JSON.stringify({ parse, opportunities }, null, 2) + "\n");
console.log(`wrote ${parse.length} parse + ${opportunities.length} opportunity cases`);
