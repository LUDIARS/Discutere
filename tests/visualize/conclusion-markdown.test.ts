import assert from "node:assert/strict";
import matter from "gray-matter";

import { renderConclusionMarkdown, safeSlug } from "../../src/visualize/conclusion-markdown.js";
import type { ConclusionDetail } from "../../src/visualize/conclusions.js";

// --- safeSlug ---
assert.equal(safeSlug("ローグライトの周回設計", "x"), "ローグライトの周回設計");
assert.equal(safeSlug("a/b:c*d?", "x"), "a-b-c-d");
assert.equal(safeSlug("", "fallback-id"), "fallback-id");
assert.equal(safeSlug("   ", "fb"), "fb");

const detail: ConclusionDetail = {
  gapId: "flow:sess-1",
  kind: "flow",
  sessionId: "sess-1",
  title: "周回設計",
  conclusion: "ビルド多様性で飽きを防ぐ",
  utteranceCount: 2,
  aufhebungCount: 1,
  updatedAt: 1700000000000,
  paper: {
    theme: "周回設計",
    tags: ["開発", "内部"],
    supplement: "社外秘の数値には触れない。",
    mechanics: [
      { name: "ビルド選択", description: "周回ごとに構成を変える", intendedAffect: "発見" },
      { name: "難度上昇", description: "周回で敵が強くなる" },
    ],
  },
  aufhebungen: ["難度と報酬の対応を保つ"],
  topOpinions: [
    { speaker: "ミナ (反対派)", content: "分岐で多様性", score: 2, source: null, sourceUrl: null },
  ],
  transcript: [
    { speaker: "陽介 (賛成派)", content: "飽きが課題", source: null, sourceUrl: null },
    { speaker: "論者#abc", content: "外部の声", source: "youtube-livechat", sourceUrl: "https://youtu.be/x" },
  ],
};

const md = renderConclusionMarkdown(detail);
const parsed = matter(md);

// frontmatter
assert.equal(parsed.data.type, "discussion-conclusion");
assert.equal(parsed.data.kind, "flow");
assert.equal(parsed.data.id, "flow:sess-1");
assert.equal(parsed.data.concluded, true);
assert.equal(parsed.data.title, "周回設計");

// 本文セクション
assert.ok(parsed.content.includes("# 周回設計"));
assert.ok(parsed.content.includes("## 結論"));
assert.ok(parsed.content.includes("ビルド多様性で飽きを防ぐ"));
assert.ok(parsed.content.includes("## 止揚ストック"));
assert.ok(parsed.content.includes("難度と報酬の対応を保つ"));
assert.ok(parsed.content.includes("## 高評価意見"));
assert.ok(parsed.content.includes("+2 ミナ (反対派)"));
assert.ok(parsed.content.includes("## 議論ログ (2 発話)"));
// 出所リンク付与
assert.ok(parsed.content.includes("([youtube-livechat](https://youtu.be/x))"));

// ディスカッションペーパー (新フロー結論のみ)
assert.ok(parsed.content.includes("## ディスカッションペーパー"));
assert.ok(parsed.content.includes("観点タグ: 開発 / 内部"));
assert.ok(parsed.content.includes("### 観点補足"));
assert.ok(parsed.content.includes("社外秘の数値には触れない。"));
assert.ok(parsed.content.includes("### ゲームのメカニクス"));
assert.ok(parsed.content.includes("**ビルド選択**: 周回ごとに構成を変える → 期待感情: 発見"));

// paper=null (旧 design_gap 議論) はペーパー節を出さない
const noPaper = renderConclusionMarkdown({ ...detail, paper: null });
assert.ok(!matter(noPaper).content.includes("## ディスカッションペーパー"));

// 結論未生成 (null) は (まとめ未生成) になり concluded=false
const noConc = renderConclusionMarkdown({ ...detail, conclusion: null });
const parsed2 = matter(noConc);
assert.equal(parsed2.data.concluded, false);
assert.ok(parsed2.content.includes("(まとめ未生成)"));

console.log("conclusion-markdown.test.ts: all passed");
