/**
 * paper-markdown: 構造化ペーパー ⇄ 正本 markdown の往復 (ハイブリッド源泉)。
 */

import assert from "node:assert/strict";

import {
  paperDraftToMarkdown,
  markdownToPaperDraft,
  renderProgressMarkdown,
  stripProgress,
  type PaperContent,
} from "../../src/flow/paper-markdown.js";

// ── paperDraftToMarkdown: 議題 / 観点補足 / メカニクスを md セクションにする ──
{
  const content: PaperContent = {
    theme: "スイカゲーム",
    tags: ["開発"],
    supplement: "開発者観点で",
    mechanics: [
      { name: "落下", description: "果物を落とす", intended_affect: "爽快" },
      { name: "合体", description: "同種が合体" },
    ],
  };
  const md = paperDraftToMarkdown(content);
  assert.ok(md.startsWith("# 議題\nスイカゲーム"), "議題が先頭");
  assert.ok(md.includes("# 観点補足\n開発者観点で"));
  assert.ok(md.includes("# ゲームのメカニクス"));
  assert.ok(md.includes("- **落下**: 果物を落とす → 期待感情: 爽快"));
  assert.ok(md.includes("- **合体**: 同種が合体"), "期待感情なしは矢印を付けない");
}

// ── supplement 空なら観点補足セクションを出さない ─────────────────────────────
{
  const md = paperDraftToMarkdown({ theme: "T", tags: [], supplement: "", mechanics: [] });
  assert.ok(!md.includes("# 観点補足"), "空 supplement は節を省略");
  assert.ok(md.includes("(メカニクスデータなし)"), "0 件はプレースホルダ");
}

// ── markdownToPaperDraft: md を構造化に戻す (round-trip) ──────────────────────
{
  const content: PaperContent = {
    theme: "スイカゲーム",
    tags: ["開発", "運用"],
    supplement: "開発者観点で",
    mechanics: [{ name: "落下", description: "果物を落とす", intended_affect: "爽快" }],
  };
  const md = paperDraftToMarkdown(content);
  const back = markdownToPaperDraft(md, content);
  assert.equal(back.theme, "スイカゲーム");
  assert.deepEqual(back.tags, ["開発", "運用"], "tags は本文 md 外なので fallback 保持");
  assert.equal(back.supplement, "開発者観点で");
  assert.equal(back.mechanics.length, 1);
  assert.equal(back.mechanics[0].name, "落下");
  assert.equal(back.mechanics[0].description, "果物を落とす");
  assert.equal(back.mechanics[0].intended_affect, "爽快");
}

// ── 自由編集: 議題本文を書き換えても拾う / メカ見出しが消えたら fallback 保持 ──
{
  const fallback: PaperContent = {
    theme: "旧テーマ",
    tags: ["開発"],
    supplement: "旧補足",
    mechanics: [{ name: "M", description: "d" }],
  };
  // 議題だけ自由編集 (メカニクス見出しを残さない自由 md)。
  const edited = "# 議題\n新しいテーマ本文\n\n# 観点補足\n新補足";
  const back = markdownToPaperDraft(edited, fallback);
  assert.equal(back.theme, "新しいテーマ本文");
  assert.equal(back.supplement, "新補足");
  assert.deepEqual(back.mechanics, fallback.mechanics, "メカ見出しが無ければ fallback を保つ");
}

// ── メカ見出しはあるが 0 件 (全削除) は尊重する ──────────────────────────────
{
  const fallback: PaperContent = {
    theme: "T",
    tags: [],
    supplement: "",
    mechanics: [{ name: "M", description: "d" }],
  };
  const edited = "# 議題\nT\n\n# ゲームのメカニクス\n(メカニクスデータなし)";
  const back = markdownToPaperDraft(edited, fallback);
  assert.deepEqual(back.mechanics, [], "メカ見出しがあって 0 件なら全削除を尊重");
}

// ── renderProgressMarkdown: base + 議論の経過 + 結論 を焼く (ライブ更新) ───────
{
  const base = "# 議題\nスイカゲーム\n\n# ゲームのメカニクス\n- **落下**: 果物を落とす";
  // 経過なし → base のまま
  assert.equal(renderProgressMarkdown(base, []), base.replace(/\s+$/g, ""));

  const rounds = [
    { round: 1, summary: "落下の爽快感が支持された", aufhebung: ["難度と爽快の両立"] },
    { round: 2, summary: "合体の戦略性が論点", aufhebung: [] },
  ];
  const md1 = renderProgressMarkdown(base, rounds);
  assert.ok(md1.includes("# 議論の経過"));
  assert.ok(md1.includes("## ラウンド 1"));
  assert.ok(md1.includes("落下の爽快感が支持された"));
  assert.ok(md1.includes("**止揚 (アウフヘーベン)**"));
  assert.ok(md1.includes("- 難度と爽快の両立"));
  assert.ok(md1.includes("## ラウンド 2"));
  assert.ok(md1.startsWith("# 議題"), "base ブリーフが先頭");

  // 冪等: 経過込みの md を base として渡しても二重化しない (stripProgress で土台化)。
  const md2 = renderProgressMarkdown(md1, rounds);
  assert.equal(md2, md1, "経過込み md を渡しても再焼きで同一");
  assert.equal((md2.match(/# 議論の経過/g) || []).length, 1, "経過節は 1 つだけ");

  // 結論を足す
  const md3 = renderProgressMarkdown(base, rounds, "落下が核・合体は戦略層");
  assert.ok(md3.includes("# 結論\n落下が核・合体は戦略層"));
}

// ── stripProgress: 経過以降を除去して base ブリーフに戻す ─────────────────────
{
  const base = "# 議題\nT\n\n# ゲームのメカニクス\n- **M**: d";
  const withProgress = base + "\n\n# 議論の経過\n\n## ラウンド 1\nまとめ";
  assert.equal(stripProgress(withProgress), base);
  assert.equal(stripProgress(base), base, "経過が無ければそのまま");
}

console.log("paper-markdown.test.ts: all passed");
