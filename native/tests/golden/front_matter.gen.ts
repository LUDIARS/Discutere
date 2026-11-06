// gray-matter (Node 実装) の入出力を記録する。実行: npx tsx native/tests/golden/front_matter.gen.ts
// 出力: native/tests/fixtures/front_matter/parse.json ({ input, ok, data, content, error? }[])
import fs from "node:fs";
import path from "node:path";
import matter from "gray-matter";

const cases: string[] = [
  "",
  "no front matter\n",
  "---\ntitle: Hello\n---\nbody\n",
  "---\nid: my-game\ntitle: \"Quoted: title\"\ngenre: action\ncount: 3\nratio: 0.5\nflag: true\nnothing: ~\nempty:\n---\n\n# Body\n",
  "---\nmechanics:\n  - name: ガチャ\n    description: 確率で入手\n    intended_aspects: [luck, collect]\n  - name: 協力\n    source: data\n---\ntext",
  "---\nlist:\n- a\n- b\n- 'c d'\nnested:\n  deep:\n    k: v\n---\n",
  "---\nlit: |\n  line1\n  line2\n\nfold: >-\n  a\n  b\n\n  c\nkeep: |+\n  x\n\n---\n",
  "---\nplain: this is\n  folded text\nhex: 0x1F\noct: 017\nneg: -12\nflt: 1e3\ninf: .inf\ndate: 2026-10-29\nts: 2026-10-29T12:00:00Z\nbool2: False\nyes: yes\n---\n",
  "---\nsingle: 'it''s'\ndouble: \"tab\\tnl\\nuni\\u3042\"\nhash: a #comment\nurl: http://example.com/a#b\ncolon: a:b\n---\n",
  "---\nflow: {a: 1, b: [x, y], c: \"q\"}\nemptyl: []\nemptym: {}\n---\n",
  "---\n# only comment\n---\nbody",
  "---\ntitle: x\n",
  "﻿---\ntitle: bom\n---\nB",
  "---\nkey: [1,\n  2, 3]\n---\n",
  "----\nnot: matter\n----\n",
  "---\nseq_of_seq:\n  - - a\n    - b\n  - - c\n---\n",
];

const out = cases.map((input) => {
  try {
    // gray-matter はキャッシュを持つので毎回別オブジェクトとして読む
    const r = matter(input, {});
    return { input, ok: true, data: JSON.parse(JSON.stringify(r.data)), content: r.content };
  } catch (e) {
    return { input, ok: false, error: (e as Error).message };
  }
});
const dir = path.join(path.dirname(new URL(import.meta.url).pathname), "..", "fixtures", "front_matter");
fs.mkdirSync(dir, { recursive: true });
fs.writeFileSync(path.join(dir, "parse.json"), JSON.stringify(out, null, 2) + "\n");
console.log(`wrote ${out.length} cases`);
