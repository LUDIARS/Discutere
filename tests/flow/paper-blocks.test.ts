/**
 * paper-blocks: 本文 md のブロック分解 / 再結合 / 差し替え (Notion 風編集の基盤)。
 */

import assert from "node:assert/strict";

import {
  splitBlocks,
  joinBlocks,
  replaceBlock,
  insertBlockAfter,
  getBlockText,
} from "../../src/flow/paper-blocks.js";

const MD = "# 議題\nスイカゲーム\n\n# ゲームのメカニクス\n- **落下**: 果物を落とす\n- **合体**: 同種が合体";

// ── splitBlocks: 見出し / 段落 / 箇条書きに分解 ──────────────────────────────
{
  const blocks = splitBlocks(MD);
  assert.equal(blocks.length, 4, "見出し2 + 段落1 + 箇条書き1");
  assert.equal(blocks[0].type, "heading");
  assert.equal(blocks[0].text, "# 議題");
  assert.equal(blocks[1].type, "paragraph");
  assert.equal(blocks[1].text, "スイカゲーム");
  assert.equal(blocks[2].type, "heading");
  assert.equal(blocks[3].type, "list");
  assert.ok(blocks[3].text.includes("- **落下**"));
  assert.ok(blocks[3].text.includes("- **合体**"), "連続箇条書きは 1 ブロック");
  assert.deepEqual(blocks.map((b) => b.id), ["b0", "b1", "b2", "b3"], "id は出現順");
}

// ── getBlockText ─────────────────────────────────────────────────────────────
{
  assert.equal(getBlockText(MD, "b1"), "スイカゲーム");
  assert.equal(getBlockText(MD, "b999"), null);
}

// ── replaceBlock: 本文差し替え / 空で削除 ────────────────────────────────────
{
  const replaced = replaceBlock(MD, "b1", "スイカゲーム (落ち物パズル)");
  assert.ok(replaced.includes("スイカゲーム (落ち物パズル)"));
  assert.ok(!replaced.includes("\nスイカゲーム\n"), "旧本文は消える");

  const deleted = replaceBlock(MD, "b1", "  ");
  const blocks = splitBlocks(deleted);
  assert.equal(blocks.length, 3, "空テキストでブロック削除");
  assert.equal(replaceBlock(MD, "b999", "x"), MD, "不在 id は無変更");
}

// ── insertBlockAfter ─────────────────────────────────────────────────────────
{
  const inserted = insertBlockAfter(MD, "b1", "補足段落");
  const blocks = splitBlocks(inserted);
  assert.equal(blocks.length, 5);
  assert.equal(blocks[2].text, "補足段落", "指定ブロック直後に挿入");
  assert.equal(insertBlockAfter(MD, "b1", "   "), MD, "空挿入は無変更");
}

// ── joinBlocks: 空ブロックを捨てて空行 1 つで連結 ────────────────────────────
{
  const md = joinBlocks([{ text: "A" }, { text: "  " }, { text: "B" }]);
  assert.equal(md, "A\n\nB");
}

// ── round-trip: split → join で意味的に保たれる ──────────────────────────────
{
  const rejoined = joinBlocks(splitBlocks(MD));
  const blocks = splitBlocks(rejoined);
  assert.equal(blocks.length, 4, "再分解しても 4 ブロック");
  assert.equal(getBlockText(rejoined, "b1"), "スイカゲーム");
}

console.log("paper-blocks.test.ts: all passed");
