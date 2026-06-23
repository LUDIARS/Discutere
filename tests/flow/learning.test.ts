/**
 * T5 学習フロー テスト。
 * - ユーザ意見 (匿名) を極性付きで KG に記録 → 議論フローの調査が読める
 * - メカニクスを data/games/<slug>.md に記録 → investigate.loadMechanics が読める
 * - LLM 議論 (ペルソナ発話/ラウンド) を呼ばない (感情カスケードも Tier 0 で完結 → LLM 0 回)
 * - 個人データ (author 名/ID) を保存しない
 */

import assert from "node:assert/strict";
import path from "node:path";
import fs from "node:fs";

import { createCore } from "../../src/core/index.js";
import { MockLLMClient } from "../../src/persona-engine/llm/mock.js";
import { runLearningFlow } from "../../src/flow/learning.js";
import { investigateTheme } from "../../src/flow/investigate.js";

const TMP = path.resolve(".tmp/flow-learning-test");
fs.rmSync(TMP, { recursive: true, force: true });
fs.mkdirSync(TMP, { recursive: true });

const gamesDir = path.join(TMP, "games");
const core = createCore(path.join(TMP, "db.sqlite"), path.join(TMP, "events.jsonl"));

// 感情カスケードに「呼ばれたら throw」する LLM を main に渡す。
// 意見は強い辞書ヒットで Tier 0 解決するため、LLM には到達しない (= LLM 議論なし)。
const guardLlm = new MockLLMClient([
  () => {
    throw new Error("LLM must NOT be called in learning flow");
  },
]);

const result = await runLearningFlow("Test Game", "Test Game の面白さ", {
  core,
  slug: "test-game",
  gamesDir,
  sentimentClients: { main: guardLlm },
  opinions: [
    { content: "このゲームは面白い、神ゲー" },
    { content: "つまらない、最悪だった" },
  ],
  mechanics: [
    {
      name: "連鎖コンボ",
      description: "連鎖でスコアが伸びる",
      intended_affect: "爽快感",
      intended_valence: "positive",
      intended_aspects: ["fun"],
      intended_emotions: ["joy"],
    },
  ],
});

// ── 記録件数 + 極性 ─────────────────────────────────────────────────────────
assert.equal(result.mechanicsRecorded, 1, "メカニクス 1 件記録");
assert.equal(result.opinionsRecorded, 2, "意見 2 件記録");
assert.equal(result.polarityBreakdown.positive, 1, "positive 1 件");
assert.equal(result.polarityBreakdown.negative, 1, "negative 1 件");
console.log("  [ok] learning: 意見/メカニクス記録 + 極性付与");

// ── LLM 議論を呼ばない (Tier 0 で完結、guard LLM は未使用) ──────────────────
assert.equal(guardLlm.calls, 0, "学習フローは LLM を呼ばない (ペルソナ発話/ラウンドなし)");
console.log("  [ok] learning: LLM 議論を呼ばない (LLM 呼び出し 0 回)");

// ── 議論フローの調査がメカニクスを読める ────────────────────────────────────
const investigation = await investigateTheme({ theme: "Test Game", tags: [], gamesDir });
const found = investigation.mechanics.find((m) => m.name === "連鎖コンボ");
assert.ok(found, "investigate が学習記録メカニクスを読める");
assert.equal(found?.intended_affect, "爽快感", "intended_affect も読める");
assert.deepEqual(found?.intended_aspects, ["fun"], "intended_aspects も読める");
console.log("  [ok] learning→discussion: investigate.loadMechanics が読める");

// ── 意見が KG に入り、議論フローの read 経路 (speaker_id ext:%) で拾える ──────
const voices = core.client.raw
  .prepare("SELECT id, speaker_id, raw_content FROM utterances WHERE speaker_id LIKE 'ext:feedback:%' ORDER BY posted_at")
  .all() as Array<{ id: string; speaker_id: string; raw_content: string }>;
assert.equal(voices.length, 2, "KG に意見 2 件 (ext:feedback)");
assert.ok(
  voices.some((v) => v.raw_content.includes("神ゲー")),
  "意見本文が KG に保存される"
);
console.log("  [ok] learning: 意見が議論フローの read 経路 (ext:%) に乗る");

// ── 個人データ非保管: speaker_id は匿名固定、author 名は保存されない ──────────
assert.ok(
  voices.every((v) => v.speaker_id === "ext:feedback:anon"),
  "speaker_id は匿名 (個人アンカーを持たない)"
);
const cols = core.client.raw.prepare("PRAGMA table_info(utterances)").all() as Array<{ name: string }>;
assert.ok(!cols.some((c) => /author|name/i.test(c.name)), "utterances に author 名カラムが無い");
console.log("  [ok] learning: 個人データ (author) を保存しない");

// ── 日本語タイトル + slug 未指定でも 500 にならない (deriveSlug フォールバック) ──
// 旧実装は toSlug("モンスターストライク")="" → throw → web /api/flow/start が 500。
const ja = await runLearningFlow("モンスターストライク", "モンスターストライクの面白さ", {
  core,
  gamesDir,
  sentimentClients: { main: guardLlm },
  opinions: [{ content: "モンストは引っ張りが楽しい、神ゲー" }],
});
assert.equal(ja.gameSlug, "モンスターストライク", "日本語タイトルは deriveSlug で非空 slug になる");
assert.equal(ja.opinionsRecorded, 1, "日本語 slug でも意見を記録できる (旧実装は slug 空で throw)");
console.log("  [ok] learning: 日本語タイトル + slug 未指定でも throw しない (500 修正)");

console.log("learning (T5) tests: all passed");
