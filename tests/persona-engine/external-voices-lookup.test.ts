/**
 * 外部の声検索の取りこぼし修正 (fix: external-voices lookup)。
 *
 *  - extractKeyTerms: フル議題文を検索キーワードに分解する (1 語 includes 照合の 0 件化を回避)。
 *  - listRelevantExternalVoices: 関連語があれば **全件 LIKE 照合** で拾う
 *    (直近 N 件しか見ない旧 CANDIDATE_CAP の盲点 = 新着に埋もれた材料が 0 件になる事故の修正)。
 */
import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";

import { createCore } from "../../src/core/index.js";
import { _resetConfig } from "../../src/config.js";
import {
  _resetQueryEmbedCache,
  normalizeQueryText,
  warmQueryEmbedding,
} from "../../src/core/vectors/query-embed-cache.js";
import { createDiscatierContextProvider } from "../../src/discatier-engine-adapter/index.js";
import { extractKeyTerms } from "../../src/discatier-engine-adapter/keyword-terms.js";
import { searchExternalVoiceRows } from "../../src/discatier-engine-adapter/voice-search.js";

// ── extractKeyTerms (純関数) ─────────────────────────────────────────────────
{
  const t1 = extractKeyTerms(["モンスターストライクの引っ張りアクションは面白いか"]);
  assert.ok(t1.includes("モンスターストライク"), "ゲーム名が 1 語として残る");
  assert.ok(t1.includes("引っ張りアクション"), "主要語が助詞で分割される");
  assert.ok(!t1.includes("面白いか"), "定型語 (面白いか) は落とす");

  assert.deepEqual(extractKeyTerms(["ボス戦"]), ["ボス戦"], "短い 1 語はそのまま");
  assert.deepEqual(extractKeyTerms([""]), [], "空文字は語なし");
  assert.deepEqual(extractKeyTerms(["の は が"]), [], "助詞のみは語なし");

  const en = extractKeyTerms(["Elden Ring の評価"]);
  assert.ok(en.includes("elden") && en.includes("ring"), "英語は小文字化して分割");
  assert.ok(en.includes("評価"), "和英混在も拾う");

  assert.ok(extractKeyTerms(["あ あ あ", "ボス戦 ボス戦"]).length <= 6, "max でクランプ");
}
console.log("ok extractKeyTerms");

// ── listRelevantExternalVoices: 新着に埋もれた材料も全件 LIKE で拾う ──────────
{
  const workDir = path.resolve(".tmp/external-voices-lookup");
  fs.rmSync(workDir, { recursive: true, force: true });
  fs.mkdirSync(workDir, { recursive: true });
  const core = createCore(path.join(workDir, "kg.kuzu"), path.join(workDir, "events.jsonl"));
  const ws = "knowledge";

  // 関連材料を「最古」に置く (postedAt 最小)。
  const hitId = core.repos.utterance.create({
    workspaceId: ws,
    sessionId: "ext:niconico:vidA",
    speakerId: "ext:niconico:userA",
    rawContent: "ゼルダの新作は探索が神ゲーだと思う",
    postedAt: 1_000,
  });
  // その後に「関連しない新着」を 310 件積む (旧実装の直近 300 件 CAP を溢れさせる)。
  for (let i = 0; i < 310; i++) {
    core.repos.utterance.create({
      workspaceId: ws,
      sessionId: "ext:niconico:vidB",
      speakerId: "ext:niconico:userB",
      rawContent: `無関係な雑談コメント番号 ${i}`,
      postedAt: 10_000 + i,
    });
  }

  const adapter = createDiscatierContextProvider(core);
  // フル議題文 (ゼルダの伝説…) を投げる: 旧実装は (a) 直近 300 件に hit が無い + (b) 全文 1 語照合で 0 件。
  const voices = adapter.listRelevantExternalVoices!(ws, ["ゼルダの伝説は面白いか"], 6);

  assert.ok(voices.length >= 1, "新着 310 件に埋もれても関連材料を拾える (CANDIDATE_CAP 盲点の修正)");
  assert.ok(
    voices.some((v) => v.id === hitId && v.content.includes("ゼルダ")),
    "フル議題文から分解した語 (ゼルダ) で最古の材料に一致する"
  );
  assert.ok(
    voices.every((v) => !v.content.includes("無関係")),
    "関連語に当たらない新着雑談は混ざらない"
  );

  core.close();
  console.log("ok listRelevantExternalVoices full-scan keyword match");
}

// ── 部分的な埋め込み索引ではキーワード順位へ安全に degrade する ──────────────
{
  const workDir = path.resolve(".tmp/external-voices-partial-vectors");
  fs.rmSync(workDir, { recursive: true, force: true });
  fs.mkdirSync(workDir, { recursive: true });
  const core = createCore(path.join(workDir, "kg.kuzu"), path.join(workDir, "events.jsonl"));
  const ws = "knowledge";
  const keywordTop = core.repos.utterance.create({
    workspaceId: ws,
    sessionId: "ext:test:partial",
    speakerId: "ext:test:keyword-top",
    rawContent: "経験値とXPの両方が成長実感につながる",
    postedAt: 3_000,
  });
  const embeddedLow1 = core.repos.utterance.create({
    workspaceId: ws,
    sessionId: "ext:test:partial",
    speakerId: "ext:test:embedded-1",
    rawContent: "経験値だけでは単調に感じる",
    postedAt: 2_000,
  });
  const embeddedLow2 = core.repos.utterance.create({
    workspaceId: ws,
    sessionId: "ext:test:partial",
    speakerId: "ext:test:embedded-2",
    rawContent: "XPの表示方法を変えてほしい",
    postedAt: 1_000,
  });
  core.vectors.registerEmbedding({
    workspaceId: ws,
    nodeType: "utterance",
    nodeId: embeddedLow1,
    vector: [1, 0],
  });
  core.vectors.registerEmbedding({
    workspaceId: ws,
    nodeType: "utterance",
    nodeId: embeddedLow2,
    vector: [0.9, 0.1],
  });

  process.env.DISCUTERE_EMBEDDING_ENABLED = "1";
  process.env.DISCUTERE_EMBEDDING_MODEL = "partial-test-model";
  _resetConfig();
  _resetQueryEmbedCache();
  await warmQueryEmbedding(
    core.client.raw,
    { embed: async () => [[1, 0]] },
    "partial-test-model",
    normalizeQueryText(["経験値"]),
    () => undefined
  );
  const rows = searchExternalVoiceRows(core, ws, ["経験値"], 1);
  assert.equal(rows[0]?.id, keywordTop, "部分索引の有無だけでキーワード最上位を落とさない");

  core.vectors.registerEmbedding({
    workspaceId: ws,
    nodeType: "utterance",
    nodeId: keywordTop,
    vector: [0, 1],
  });
  const hybridRows = searchExternalVoiceRows(core, ws, ["経験値"], 1);
  assert.equal(hybridRows[0]?.id, embeddedLow1, "索引完成後はクエリ類似度を RRF 順位へ反映する");

  delete process.env.DISCUTERE_EMBEDDING_ENABLED;
  delete process.env.DISCUTERE_EMBEDDING_MODEL;
  _resetConfig();
  _resetQueryEmbedCache();
  core.close();
  fs.rmSync(workDir, { recursive: true, force: true });
  console.log("ok partial vector coverage degrades to keyword ranking");
}

console.log("external-voices-lookup tests: all passed");
