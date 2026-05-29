import assert from "node:assert/strict";

import {
  extractWikilinks,
  formatWikilink,
  normalizeType,
  parseWikilink,
  TYPE_TO_DIR,
  TYPE_TO_TABLE,
} from "../../src/visualize/index.js";

// parse
{
  const link = parseWikilink("[[hyp:abc123]]");
  assert.deepEqual(link, { type: "hyp", id: "abc123" });
  console.log("ok parse hyp");
}
{
  // 周辺の空白は trim 許容
  const link = parseWikilink("  [[utt:550e8400]]  ");
  assert.deepEqual(link, { type: "utt", id: "550e8400" });
  console.log("ok parse with trim");
}
{
  // 未知 prefix → null
  assert.equal(parseWikilink("[[unknown:foo]]"), null);
  // ノイズ混在 → 単独 parse 失敗
  assert.equal(parseWikilink("text [[hyp:x]] tail"), null);
  // 空 id → null
  assert.equal(parseWikilink("[[hyp:]]"), null);
  console.log("ok parse failures");
}

// format
{
  assert.equal(formatWikilink({ type: "mch", id: "ghi789" }), "[[mch:ghi789]]");
  assert.throws(() => formatWikilink({ type: "xxx" as any, id: "a" }));
  assert.throws(() => formatWikilink({ type: "hyp", id: "has space" }));
  console.log("ok format + reject");
}

// extract (本文内の wikilink を全部拾う)
{
  const text = `# h\n- [[hyp:a1]]\n- [[utt:b2]]\nrandom [[bad: x]] [[hyp:c3]] tail`;
  const found = extractWikilinks(text);
  assert.deepEqual(found, [
    { type: "hyp", id: "a1" },
    { type: "utt", id: "b2" },
    { type: "hyp", id: "c3" },
  ]);
  console.log("ok extract from mixed text");
}

// normalizeType (full word / prefix 両方受ける)
{
  assert.equal(normalizeType("hypothesis"), "hyp");
  assert.equal(normalizeType("HYP"), "hyp");
  assert.equal(normalizeType("design-gap"), "gap");
  assert.equal(normalizeType("designgap"), "gap");
  assert.equal(normalizeType("gap"), "gap");
  assert.equal(normalizeType("session"), "ses");
  assert.equal(normalizeType("ses"), "ses");
  assert.equal(normalizeType("nope"), null);
  console.log("ok normalizeType");
}

// type ↔ table / dir のマップ整合性
{
  assert.equal(TYPE_TO_TABLE.hyp, "hypotheses");
  assert.equal(TYPE_TO_TABLE.gap, "design_gaps");
  assert.equal(TYPE_TO_TABLE.mch, "mechanics");
  assert.equal(TYPE_TO_DIR.hyp, "hypothesis");
  assert.equal(TYPE_TO_DIR.utt, "utterance");
  console.log("ok type mappings");
}

console.log("wikilink.test.ts: all passed");
