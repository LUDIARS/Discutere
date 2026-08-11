import assert from "node:assert/strict";

import { PaperAutoStartInputError, paperDraftForAutoStart } from "../../src/flow/paper-auto-start.js";

{
  const draft = paperDraftForAutoStart({
    bodyMd: "# 議題\n循環経済は長期的な面白さを生むか\n\n# 論点\n1. 複雑さは奥深さにつながるか",
    theme: "フォールバック",
    tags: ["開発"],
  });
  assert.equal(draft.theme, "循環経済は長期的な面白さを生むか", "標準の議題見出しをテーマにする");
  assert.deepEqual(draft.issues, ["複雑さは奥深さにつながるか"], "標準ペーパーの論点を派生する");
  assert.deepEqual(draft.tags, ["開発"], "本文外のタグを保持する");
  assert.ok(draft.bodyMd.includes("# 論点"), "入力 Markdown を正本として保持する");
}

{
  const draft = paperDraftForAutoStart({ bodyMd: "# MagicChronicle レビュー討議\n\n独自形式の本文" });
  assert.equal(draft.theme, "MagicChronicle レビュー討議", "独自ペーパーは先頭 H1 をテーマにする");
  assert.equal(draft.bodyMd, "# MagicChronicle レビュー討議\n\n独自形式の本文");
}

assert.throws(
  () => paperDraftForAutoStart({ bodyMd: "   " }),
  PaperAutoStartInputError,
  "空のペーパーは拒否する"
);
assert.throws(
  () => paperDraftForAutoStart({ bodyMd: "見出しのない本文" }),
  PaperAutoStartInputError,
  "テーマも H1 も無いペーパーは拒否する"
);

console.log("paper auto start tests: all passed");
