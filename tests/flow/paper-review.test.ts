/**
 * ペーパーレビューゲート (議論開始前の確認・調整) のテスト。
 *   - 共有コア (paper-review.ts): 草案構築 / 自然文編集 / 正規化 / 承認語判定 / 表示。
 *   - director の paperOverride: 確定ペーパーが investigate を上書きして永続されること。
 */

import assert from "node:assert/strict";
import os from "node:os";
import path from "node:path";

import {
  buildPaperDraft,
  applyPaperEdit,
  coercePaperDraft,
  renderPaperReview,
  isApprovalText,
  type PaperDraft,
} from "../../src/flow/paper-review.js";
import { MockLLMClient } from "../../src/persona-engine/llm/mock.js";

const NONEXISTENT_GAMES = path.join(os.tmpdir(), "discutere-no-such-games-dir");

// ── buildPaperDraft ──────────────────────────────────────────────────────────
{
  const voices = [
    { content: "周回が楽しい", source: "niconico" },
    { content: "ビルドの幅がある", source: "youtube" },
    { content: "難度が理不尽", source: "steam" },
    { content: "4 件目 (サンプル外)", source: "niconico" },
  ];
  const { draft, info } = await buildPaperDraft("ローグライト", ["開発"], {
    gamesDir: NONEXISTENT_GAMES, // メカニクスは空 (investigate は外部呼びなし)
    listExternalVoices: () => voices,
  });
  assert.equal(draft.theme, "ローグライト");
  assert.deepEqual(draft.tags, ["開発"]);
  assert.ok(draft.supplement.includes("開発者観点"), "観点補足がタグから生成される");
  assert.deepEqual(draft.mechanics, []);
  assert.equal(info.voiceCount, 4);
  assert.equal(info.countCapped, false);
  assert.equal(info.samples.length, 3, "サンプルは 3 件まで");
  assert.equal(info.samples[0].source, "niconico");
}

// ── applyPaperEdit: 正常 (JSON 反映) ─────────────────────────────────────────
{
  const base: PaperDraft = { theme: "T", tags: ["開発"], supplement: "S", mechanics: [] };
  const llm = new MockLLMClient([
    () =>
      JSON.stringify({
        theme: "T2",
        tags: ["開発", "運用"],
        supplement: "S2",
        mechanics: [{ name: "ガチャ", description: "確率で入手", intended_affect: "射幸" }],
        changeSummary: "メカニクスにガチャを追加",
      }),
  ]);
  const r = await applyPaperEdit(base, "メカニクスにガチャを追加", llm);
  assert.equal(r.applied, true);
  assert.equal(r.draft.theme, "T2");
  assert.deepEqual(r.draft.tags, ["開発", "運用"]);
  assert.equal(r.draft.mechanics.length, 1);
  assert.equal(r.draft.mechanics[0].name, "ガチャ");
  assert.equal(r.changeSummary, "メカニクスにガチャを追加");
}

// ── applyPaperEdit: コードフェンス混入でも JSON 抽出 ────────────────────────
{
  const base: PaperDraft = { theme: "T", tags: [], supplement: "S", mechanics: [] };
  const llm = new MockLLMClient([
    () => "はい、更新します:\n```json\n{\"theme\":\"T\",\"supplement\":\"S3\",\"mechanics\":[],\"changeSummary\":\"補足変更\"}\n```",
  ]);
  const r = await applyPaperEdit(base, "観点補足を変えて", llm);
  assert.equal(r.applied, true);
  assert.equal(r.draft.supplement, "S3");
}

// ── applyPaperEdit: パース不能なら元のまま applied=false ─────────────────────
{
  const base: PaperDraft = { theme: "T", tags: ["開発"], supplement: "S", mechanics: [{ name: "M", description: "d" }] };
  const llm = new MockLLMClient([() => "ごめんなさい、よく分かりませんでした"]);
  const r = await applyPaperEdit(base, "曖昧な指示", llm);
  assert.equal(r.applied, false);
  assert.deepEqual(r.draft, base, "失敗時はドラフトを壊さない");
}

// ── applyPaperEdit: 不正タグは除去される ─────────────────────────────────────
{
  const base: PaperDraft = { theme: "T", tags: [], supplement: "", mechanics: [] };
  const llm = new MockLLMClient([
    () => JSON.stringify({ theme: "T", tags: ["開発", "存在しないタグ", "機密"], supplement: "", mechanics: [], changeSummary: "tag" }),
  ]);
  const r = await applyPaperEdit(base, "タグ調整", llm);
  assert.deepEqual(r.draft.tags, ["開発", "機密"], "有効タグのみ残る");
}

// ── coercePaperDraft: Web フォームの直接編集を正規化 ────────────────────────
{
  const fallback: PaperDraft = { theme: "FB", tags: ["開発"], supplement: "FBS", mechanics: [{ name: "FM", description: "d" }] };
  const coerced = coercePaperDraft(
    { theme: "  新議題  ", tags: ["運用", "bad"], mechanics: [{ name: "A", description: "x" }, { description: "名前なしは除外" }] },
    fallback
  );
  assert.equal(coerced.theme, "新議題");
  assert.deepEqual(coerced.tags, ["運用"]);
  assert.equal(coerced.supplement, "FBS", "supplement 欠落は fallback");
  assert.equal(coerced.mechanics.length, 1, "name なしメカニクスは除外");
  // 不正入力はまるごと fallback
  assert.deepEqual(coercePaperDraft(null, fallback), fallback);
}

// ── isApprovalText ───────────────────────────────────────────────────────────
{
  for (const t of ["開始", "承認", "OK", "ok。", "これで開始", "✅", "go"]) {
    assert.equal(isApprovalText(t), true, `${t} は承認語`);
  }
  for (const t of ["", "メカニクスにガチャを追加", "もっと否定的に", "開始したい理由は"]) {
    assert.equal(isApprovalText(t), false, `${t} は承認語でない`);
  }
}

// ── renderPaperReview ────────────────────────────────────────────────────────
{
  const draft: PaperDraft = {
    theme: "ローグライト",
    tags: ["開発"],
    supplement: "開発者観点で",
    mechanics: [{ name: "ビルド選択", description: "構成を変える", intended_affect: "発見" }],
  };
  const md = renderPaperReview(draft, { voiceCount: 50, countCapped: true, samples: [{ content: "楽しい", source: "niconico" }] });
  assert.ok(md.includes("ローグライト"));
  assert.ok(md.includes("ビルド選択"));
  assert.ok(md.includes("期待感情: 発見"));
  assert.ok(md.includes("外部の声 50+ 件"), "頭打ちは + 付き");
  assert.ok(md.includes("niconico"));
}

// ── director: paperOverride が investigate を上書きして永続される ────────────
{
  process.env.DATABASE_PATH = path.join(os.tmpdir(), `discutere-paper-override-${process.pid}.db`);
  const { _resetFlowDb, getFlowDb } = await import("../../src/flow/db/connection.js");
  _resetFlowDb();
  const { runDiscussionFlow } = await import("../../src/flow/director.js");

  const llm = new MockLLMClient([], '{"ok":true}');
  const result = await runDiscussionFlow("確定ペーパーのテーマ", ["開発"], {
    llm,
    rounds: 1,
    turnsPerRound: 1,
    gamesDir: NONEXISTENT_GAMES,
    possess: false,
    paperOverride: {
      mechanics: [{ name: "OVERRIDE_MECH", description: "人間が直したメカニクス", intended_affect: "納得" }],
      supplement: "OVERRIDE_SUPPLEMENT",
    },
    log: () => {},
    warn: () => {},
  });

  const db = getFlowDb();
  const row = db
    .prepare("SELECT theme, mechanics_json, supplement FROM discussion_paper WHERE id = ?")
    .get(result.paperId) as { theme: string; mechanics_json: string; supplement: string };
  assert.equal(row.theme, "確定ペーパーのテーマ");
  assert.equal(row.supplement, "OVERRIDE_SUPPLEMENT", "確定ペーパーの観点補足が永続される");
  const mechs = JSON.parse(row.mechanics_json) as Array<{ name: string }>;
  assert.equal(mechs.length, 1);
  assert.equal(mechs[0].name, "OVERRIDE_MECH", "investigate ではなく override のメカニクスが永続される");

  delete process.env.DATABASE_PATH;
}

console.log("paper-review.test.ts: all passed");
