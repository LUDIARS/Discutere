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
import { createDiscatierContextProvider } from "../../src/discatier-engine-adapter/index.js";
import { extractKeyTerms } from "../../src/discatier-engine-adapter/keyword-terms.js";

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

console.log("external-voices-lookup tests: all passed");
