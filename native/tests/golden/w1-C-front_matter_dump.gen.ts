// gray-matter 4.0.3 の matter.stringify / js-yaml 3.15 safeDump の出力を記録する。
// 実行: npx tsx native/tests/golden/w1-C-front_matter_dump.gen.ts
// 出力: native/tests/fixtures/front_matter/stringify.json ({ content, data, output, yaml }[])
import fs from "node:fs";
import path from "node:path";
import { createRequire } from "node:module";
import matter from "gray-matter";

const require = createRequire(import.meta.url);
// gray-matter が実際に使う js-yaml (3.15.2) を使う。
const yaml = require("gray-matter/node_modules/js-yaml") as { safeDump: (v: unknown) => string };

const long = "これは長い説明文です。".repeat(12);
const longEn = Array.from({ length: 30 }, (_, i) => `word${i}`).join(" ");
const longEnSpaces = "alpha  beta   gamma ".repeat(8);
const noSpace = "x".repeat(150);

const scalars: unknown[] = [
  "plain", "日本語の値", "", " ", "  lead", "trail ", "a:b", "a: b", "key:", "a #b", "a#b", "#tag", "- item", "-x",
  "?q", ":c", ",c", "[x]", "]x", "{y}", "}y", "&a", "*a", "!a", "|a", ">a", "'q'", "\"dq\"", "%p", "@at", "`bt`",
  "it's", "say \"hi\"", "back\\slash", "tab\there", "cr\rhere", "nul\u0000x", "bell\u0007", "esc\u001b", "nbsp\u00a0x",
  "nel\u0085x", "ls\u2028x", "ps\u2029x", "bom\ufeffx", "emoji😀", "😀", "ctrl\u0001x", "del\u007fx", "c1\u0090x",
  "123", "-12", "+3", "0", "0x1F", "0b101", "017", "08", "0o17", "1_000", "_1", "1_", "1:20", "1:2:3", "1:", "1.5",
  ".5", "1.", "1e3", "1.e3", "-1.5e-3", "1E+5", ".inf", "-.Inf", "+.INF", ".nan", ".NaN", "NaN", "Infinity", "1:20.5",
  "2026-10-29", "2026-1-2", "2026-10-29T12:00:00Z", "2026-10-29 12:00:00", "2026-10-29t1:02:03.123+09:00",
  "2026-10-29T12:00:00 Z", "2026-10-29T12:00:00 ", "true", "True", "TRUE", "tRue", "false", "False", "null", "Null",
  "NULL", "~", "nULL", "yes", "Yes", "YES", "no", "No", "on", "On", "off", "OFF", "y", "Y", "n", "N", "<<", "<",
  "a\nb", "a\nb\n", "a\nb\n\n", "\n", "\n\n", "\nlead", "  indented\nnext", "line\n  more\nback", "x\n\n\ny",
  "trailing spaces  \nnext", long, longEn, longEnSpaces, noSpace, `${longEn}\n${longEn}`, `${longEn}\n\n${longEn}\n`,
  `  ${longEn}`, `${longEn}\n  indented ${longEn}\n${longEn}`, `\n${longEn}`, `${noSpace} tail`, `a ${noSpace}`,
  "http://example.com/a#b?c=d", "C:\\path", "100%", "a, b", "x - y", "x ? y", "{a: 1}", "=", "a = b",
  1, -1, 0, 3.5, -0.25, 1e21, 1e-7, 5e-324, 123456789012, 9007199254740991, 0.1, 100, true, false, null,
];

const datas: unknown[] = [
  { title: "Hello" },
  { id: "my-game", title: "Quoted: title", genre: "action", count: 3, ratio: 0.5, flag: true, nothing: null },
  { mechanics: [{ name: "ガチャ", description: "確率で入手", intended_aspects: ["luck", "collect"] }, { name: "協力", source: "data" }] },
  { list: ["a", "b", "c d"], nested: { deep: { k: "v" } } },
  { emptyArr: [], emptyObj: {}, nestedEmpty: { a: [], b: {} }, arrOfEmpty: [[], {}] },
  { seq_of_seq: [["a", "b"], ["c"], [[1, 2], []]] },
  { objs: [{ a: { b: [1, { c: "multi\nline" }] } }, { list: ["x\ny", long] }] },
  { lit: "line1\nline2", fold: long, keep: "x\n\n", keepOnly: "\n", lead: "  sp\nx" },
  { deep: { l1: { l2: { l3: { l4: { l5: { text: long, en: longEn } } } } } } },
  { arr: [long, longEn, `${longEn}\n${longEn}`] },
  { "key: colon": 1, "": "empty key", "true": "bool key", "123": "num key", "a\nb": "nl key", "-k": 1, "#k": 2, "日本語キー": "値", "it's": "x", " sp": "y" },
  { ["k".repeat(1100)]: "long key", ["キ".repeat(600)]: { nested: 1 } },
  { b: 1, "2": 2, "1": 3, a: 4, "10": 5, "01": 6, "-1": 7, "4294967295": 8, "4294967294": 9 },
  { mixed: [1, "two", null, true, 3.25, { k: null }, ["n"]] },
  { conclusion: "結論: これは「議論」の#まとめ。\n- 箇条書き\n- 二つ目\n", tags: ["機密", "内部", "未来予測"], createdAt: "2026-10-29T12:00:00.000Z" },
  ...scalars.map((v, i) => ({ [`v${i}`]: v })),
  { allScalars: scalars },
  { inSeq: scalars.filter((v) => typeof v === "string") },
];

const contents = ["", "body", "body\n", "\n# 見出し\n\n本文\n", "no newline", "\n\n", "---\nexisting: yes\nkeep: 1\n---\nrest of body\n", "\ufeffbom body"];

type Case = { content: string; data: unknown; output: string; yaml: string };
const out: Case[] = [];
const addCase = (content: string, data: unknown) => {
  // matter.stringify は data を変更しない。JSON 往復で JSON にできる値だけにする。
  const d = JSON.parse(JSON.stringify(data));
  out.push({ content, data: d, output: matter.stringify(content, d), yaml: yaml.safeDump(d) });
};
datas.forEach((d, i) => addCase(contents[i % contents.length], d));
for (const c of contents) addCase(c, {});
for (const c of contents) addCase(c, { title: "t", extra: [1] });

const dir = path.join(path.dirname(new URL(import.meta.url).pathname), "..", "fixtures", "front_matter");
fs.mkdirSync(dir, { recursive: true });
fs.writeFileSync(path.join(dir, "stringify.json"), JSON.stringify(out, null, 2) + "\n");
console.log(`wrote ${out.length} cases`);
