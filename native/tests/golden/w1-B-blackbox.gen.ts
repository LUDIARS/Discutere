// @ludiars/blackbox (Node 実装) の純関数の入出力を記録する。実行: npx tsx native/tests/golden/w1-B-blackbox.gen.ts
// 出力: native/tests/fixtures/blackbox/cases.json
import fs from "node:fs";
import path from "node:path";
import { canonicalJson, describeCondition, evaluate, ruleFingerprint, validateCondition } from "@ludiars/blackbox";

const values: unknown[] = [
  null,
  1,
  1.5,
  -0.1,
  1e21,
  "s",
  true,
  [3, 1, 2],
  { b: 1, a: { d: [1, { z: 1, y: 2 }], c: null } },
  { "10": "x", "2": "y", "b": 1, "a": 2, "01": 3, "4294967295": 4, "4294967294": 5 },
  { "é": 1, "z": 2, "😀": 3, "￿": 4, "A": 5 },
];
const canonical = values.map((value) => ({ value, json: canonicalJson(value) }));

const conditions: unknown[] = [
  { op: "cmp", feature: "x", cmp: ">=", value: 1 },
  { value: 2, cmp: "==", op: "cmp", feature: "dow" },
  { op: "in", feature: "kind", values: ["a", 2, "c"] },
  { op: "and", clauses: [{ op: "cmp", feature: "x", cmp: ">", value: 0.5 }, { op: "not", clause: { op: "cmp", feature: "f", cmp: "==", value: true } }] },
  { op: "or", clauses: [{ op: "cmp", feature: "s", cmp: "!=", value: "q" }, { op: "in", feature: "n", values: [1, 2] }] },
  { op: "xor" },
  { op: 5 },
  [],
  "str",
  null,
  { op: "and", clauses: [] },
  { op: "in", feature: "k", values: [] },
  { op: "in", feature: "k", values: [true] },
  { op: "cmp", feature: 1, cmp: ">", value: 1 },
  { op: "cmp", feature: "x", cmp: "=>", value: 1 },
  { op: "cmp", feature: "x", cmp: ">", value: null },
  { op: "not" },
  { op: "not", clause: { op: "not", clause: { op: "not", clause: { op: "not", clause: { op: "not", clause: { op: "not", clause: { op: "not", clause: { op: "not", clause: { op: "not", clause: { op: "cmp", feature: "x", cmp: ">", value: 1 } } } } } } } } } },
  { op: "not", clause: { op: "not", clause: { op: "not", clause: { op: "not", clause: { op: "not", clause: { op: "not", clause: { op: "not", clause: { op: "not", clause: { op: "cmp", feature: "x", cmp: ">", value: 1 } } } } } } } } },
];
const validation = conditions.map((input) => {
  try {
    const cond = validateCondition(input);
    return { input, ok: true, cond, text: describeCondition(cond) };
  } catch (e) {
    return { input, ok: false, error: (e as Error).message };
  }
});

const features = [
  { x: 1, kind: "a", f: false, s: "p", n: 2, dow: 2 },
  { x: 0.2, kind: 2, f: true, s: "q", n: 3, dow: "2" },
  { x: "1", kind: "c" },
  {},
];
const evals = validation
  .filter((v) => v.ok)
  .map((v) => ({ cond: (v as { cond: unknown }).cond, results: features.map((f) => evaluate((v as { cond: never }).cond, f as never)) }));

const fingerprints = [
  { when: conditions[0], output: { rain: true } },
  { when: conditions[1], output: { names: ["x"] } },
  { when: conditions[2], output: null },
].map((c) => ({ ...c, fingerprint: ruleFingerprint(c.when, c.output) }));

const out = { canonical, validation, features, evals, fingerprints };
const target = path.resolve("native/tests/fixtures/blackbox/cases.json");
fs.writeFileSync(target, JSON.stringify(out, null, 2) + "\n");
console.log(`wrote ${target}`);
