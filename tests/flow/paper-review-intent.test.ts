/**
 * ペーパー調整返信の振り分け (paper-review-intent.ts) と、種類ごとの材料集め
 * (paper-review-enrich.ts) のテスト。クロール / 検索 / メカニクス取得は注入して検証する。
 */

import assert from "node:assert/strict";
import { classifyPaperReviewIntent, isNonInstructionReply } from "../../src/flow/paper-review-intent.js";
import { buildIntentInstruction } from "../../src/flow/paper-review-enrich.js";
import type { investigateTheme } from "../../src/flow/investigate.js";

// ── 振り分け ─────────────────────────────────────────────────────────────────
{
  const a = classifyPaperReviewIntent("観点補足を初心者向けにして");
  assert.equal(a.kind, "adjust");
  assert.deepEqual(a.terms, []);

  const v = classifyPaperReviewIntent("外部の声を取り込んで「周回」「スタミナ」");
  assert.equal(v.kind, "voices");
  assert.deepEqual(v.terms, ["周回", "スタミナ"]);
  assert.equal(classifyPaperReviewIntent("ユーザーの感想を集めて").kind, "voices");
  assert.deepEqual(classifyPaperReviewIntent("口コミも入れて").terms, [], "語の指定が無ければ空 (議題で検索)");

  const s = classifyPaperReviewIntent("類似ゲーム: 「モンスト」「パズドラ」");
  assert.equal(s.kind, "similar_games");
  assert.deepEqual(s.terms, ["モンスト", "パズドラ"]);
  const loose = classifyPaperReviewIntent("メカニクスの代替として モンスト、パズドラ を参考に");
  assert.equal(loose.kind, "similar_games");
  assert.deepEqual(loose.terms, ["モンスト", "パズドラ"], "引用が無くても区切りでゲーム名を拾う");

  assert.equal(
    classifyPaperReviewIntent("類似ゲームの外部の声を集めて").kind,
    "voices",
    "両方のキーワードがあれば外部の声を優先"
  );
  console.log("  [ok] paper-review-intent: 3 種類への振り分けと対象語の抽出");
}

const voice = (content: string) => ({ content, source: "niconico" }) as never;

// ── 外部の声: 指定語でクロール → 集めた声を根拠に指示 ─────────────────────
{
  const crawled: string[] = [];
  const searched: string[][] = [];
  const r = await buildIntentInstruction(classifyPaperReviewIntent("外部の声を取り込んで「周回」"), {
    theme: "コンビニドミナント",
    tags: [],
    crawl: async (q) => {
      crawled.push(q);
      return { imported: 7 };
    },
    listExternalVoices: (terms) => {
      searched.push(terms);
      return [voice("周回がだるい"), voice("周回報酬が渋い")];
    },
  });
  assert.deepEqual(crawled, ["周回"]);
  assert.deepEqual(searched[0], ["周回"]);
  assert.ok(r.instruction?.includes("周回がだるい"), "集めた声を指示に含める");
  assert.ok(r.instruction?.includes("出所"), "出所を残すよう指示する");
  assert.match(r.notice, /7 件をクロール/);
  assert.match(r.notice, /2 件を根拠/);
  console.log("  [ok] paper-review-enrich: 外部の声は指定語でクロールして根拠に反映");
}

{
  const crawled: string[] = [];
  const r = await buildIntentInstruction(classifyPaperReviewIntent("口コミも入れて"), {
    theme: "コンビニドミナント",
    tags: [],
    crawl: async (q) => {
      crawled.push(q);
      throw new Error("network down");
    },
    listExternalVoices: () => [],
    warn: () => {},
  });
  assert.deepEqual(crawled, ["コンビニドミナント"], "語の指定が無ければ議題で検索");
  assert.equal(r.instruction, null, "声が見つからなければ反映しない");
  assert.match(r.notice, /見つかりませんでした/);
  console.log("  [ok] paper-review-enrich: 声が無ければ反映せず通知のみ (クロール失敗でも続行)");
}

// ── 類似ゲーム: 既存データのメカニクス + 無いものは一般知識で補う指示 ───────
{
  const fakeInvestigate = (async ({ theme }: { theme: string }) => ({
    mechanics:
      theme === "モンスト"
        ? [{ name: "引っ張りハンティング", description: "弾いて当てる", source: "curated" }]
        : [],
    sentimentCount: 0,
    youtubeUsed: false,
    youtubeComments: [],
  })) as unknown as typeof investigateTheme;
  const r = await buildIntentInstruction(classifyPaperReviewIntent("類似ゲーム: 「モンスト」「パズドラ」"), {
    theme: "コンビニドミナント",
    tags: [],
    investigate: fakeInvestigate,
  });
  assert.ok(r.instruction?.includes("[モンスト] 引っ張りハンティング: 弾いて当てる"));
  assert.ok(r.instruction?.includes("(参考: ゲーム名)"), "本来のメカニクスと区別する指示");
  assert.ok(r.instruction?.includes("パズドラ"), "データに無いゲームは一般知識で補う指示");
  assert.match(r.notice, /モンスト、パズドラ/);
  console.log("  [ok] paper-review-enrich: 類似ゲームのメカニクスを代替として指示");
}

{
  const r = await buildIntentInstruction(classifyPaperReviewIntent("観点補足を初心者向けに"), {
    theme: "x",
    tags: [],
  });
  assert.equal(r.instruction, "観点補足を初心者向けに", "議論内容の調整は本文をそのまま渡す");
  assert.equal(r.notice, "");
  console.log("  [ok] paper-review-enrich: 議論内容の調整はそのまま");
}

// ── 調整指示になっていない返信 (別の質問への回答・相づち) ───────────────────────
{
  for (const t of ["ない", "ない。", "なし", "特にない", "はい", "いいえ", "了解", "ありがとう", "👍", "…", "a", "  無い  "]) {
    assert.equal(isNonInstructionReply(t), true, `${t} は指示ではない`);
  }
  for (const t of ["メカニクスにガチャを追加", "観点補足を初心者向けに", "ないものを足して", "類似ゲーム「モンスト」"]) {
    assert.equal(isNonInstructionReply(t), false, `${t} は指示`);
  }
  console.log("  [ok] isNonInstructionReply: 相づち・否定・記号だけはペーパーを書き換えない");
}
