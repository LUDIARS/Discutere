// JS の /[\p{L}\p{N}]/u (文字・数字) の範囲表を Node から書き出す。
// 実行: npx tsx native/tests/golden/w1-E-unicode_ln.gen.ts
// 出力: native/src/flow/unicode_letter_number.inc (flow/paper_review_intent.cpp が読む)
import fs from "node:fs";
import path from "node:path";

const re = /[\p{L}\p{N}]/u;
const ranges: Array<[number, number]> = [];
let start = -1;
for (let cp = 0; cp <= 0x10ffff; cp++) {
  const isSurrogate = cp >= 0xd800 && cp <= 0xdfff;
  const hit = !isSurrogate && re.test(String.fromCodePoint(cp));
  if (hit && start < 0) start = cp;
  if (!hit && start >= 0) {
    ranges.push([start, cp - 1]);
    start = -1;
  }
}
if (start >= 0) ranges.push([start, 0x10ffff]);
const hex = (n: number) => "0x" + n.toString(16).toUpperCase();
const lines = [
  `// 生成物: native/tests/golden/w1-E-unicode_ln.gen.ts (Node ${process.version} の /[\\p{L}\\p{N}]/u)。手で編集しない。`,
  `// {first, last} の昇順の範囲 (${ranges.length} 個)。`,
];
for (let i = 0; i < ranges.length; i += 4) {
  lines.push(ranges.slice(i, i + 4).map(([a, b]) => `{${hex(a)}, ${hex(b)}},`).join(" "));
}
const out = path.join(path.dirname(new URL(import.meta.url).pathname), "..", "..", "src", "flow", "unicode_letter_number.inc");
fs.writeFileSync(out, lines.join("\n") + "\n");
console.log(`wrote ${ranges.length} ranges`);
