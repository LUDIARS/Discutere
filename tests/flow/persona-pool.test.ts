/**
 * ペルソナプール + ユーザ嗜好 + 嗜好近傍選定 (憑依) テスト。
 */

import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";

const TMP_DIR = path.resolve(".tmp/flow-test");
fs.mkdirSync(TMP_DIR, { recursive: true });
process.env.DATABASE_PATH = path.join(TMP_DIR, "persona-pool.db");
try {
  fs.rmSync(process.env.DATABASE_PATH, { force: true });
} catch {
  /* noop */
}

const { _resetFlowDb } = await import("../../src/flow/db/connection.js");
_resetFlowDb();

const {
  insertPoolPersona,
  getPoolPersona,
  listPoolPersonas,
  archivePoolPersona,
  upsertUserAffect,
  getUserAffect,
  bookmarkPersonaAsUser,
  selectByAffinity,
  selectPossessionByTheme,
  toFlowPersona,
} = await import("../../src/flow/persona-pool.js");
const { DIM, textToVector } = await import("../../src/flow/sentiment-vector.js");

const zeros = (): number[] => new Array(DIM as number).fill(0);

// ── insert / get / list / archive ───────────────────────────────
{
  const roguelike = { ...zeros() };
  const vecRogue = zeros();
  vecRogue[0] = 0.9; // valence 高め (例)
  vecRogue[3] = 1; // 何らかの感情次元
  const vecGacha = zeros();
  vecGacha[0] = 0.2;
  vecGacha[5] = 1;

  insertPoolPersona({
    id: "p-rogue",
    name: "ローグ好き太郎",
    role: "opinion",
    speechStyle: "ストイック",
    traits: ["高難度志向", "上達快感"],
    affectVector: vecRogue,
    origin: "generated",
    parentIds: [],
    learningSource: "roguelike",
  });
  insertPoolPersona({
    id: "p-gacha",
    name: "ソシャゲ花子",
    role: "opinion",
    speechStyle: "カジュアル",
    traits: ["収集欲", "イベント追従"],
    affectVector: vecGacha,
    origin: "generated",
    parentIds: [],
    learningSource: "gacha",
  });

  const got = getPoolPersona("p-rogue");
  assert.ok(got, "getPoolPersona");
  assert.equal(got!.name, "ローグ好き太郎");
  assert.equal(got!.affectVector.length, DIM as number, "affect dim 復元");

  const all = listPoolPersonas();
  assert.equal(all.length, 2, "list 2 件");
  assert.equal(listPoolPersonas({ learningSource: "gacha" }).length, 1, "source 絞り込み");

  archivePoolPersona("p-gacha");
  assert.equal(listPoolPersonas().length, 1, "archive で除外");
  assert.equal(getPoolPersona("p-gacha"), null, "archive 済みは get で null");
  console.log("  [ok] persona pool: insert/get/list/archive");

  // 復活させて以降の選定テストに使う
  insertPoolPersona({
    id: "p-gacha2",
    name: "ソシャゲ花子",
    role: "opinion",
    speechStyle: "カジュアル",
    traits: ["収集欲"],
    affectVector: vecGacha,
    origin: "generated",
    parentIds: [],
    learningSource: "gacha",
  });
}

// ── ユーザ嗜好 upsert / get + text 由来ベクトル ─────────────────
{
  const up = upsertUserAffect({ userKey: "u1", desiredText: "高難度をストイックに攻略して上達したい" });
  assert.equal(up.vector.length, DIM as number, "user affect dim");
  const got = getUserAffect("u1");
  assert.ok(got, "getUserAffect");
  assert.deepEqual(got!.vector, textToVector("高難度をストイックに攻略して上達したい"), "text→vector 一致");

  // 上書き (upsert)
  upsertUserAffect({ userKey: "u1", desiredText: "別の体験", label: "L" });
  assert.equal(getUserAffect("u1")!.label, "L", "upsert で更新");
  const bookmarked = bookmarkPersonaAsUser({ personaId: "p-gacha2", userKey: "self" });
  assert.equal(bookmarked.label, "ソシャゲ花子", "bookmark label");
  assert.deepEqual(bookmarked.vector, getPoolPersona("p-gacha2")!.affectVector, "bookmark uses persona vector");
  assert.ok(getUserAffect("self")!.desiredText.includes("persona_id: p-gacha2"), "bookmark desiredText includes persona id");
  console.log("  [ok] user affect: upsert/get + text 由来ベクトル");
}

// ── 憑依: ベクトル最近傍選定 ─────────────────────────────────────
{
  // ローグ好きペルソナと同方向のユーザベクトルを与えると p-rogue が選ばれる
  const v = zeros();
  v[0] = 0.95;
  v[3] = 1;
  const hits = selectByAffinity(v, 1);
  assert.equal(hits.length, 1, "k=1");
  assert.equal(hits[0].persona.id, "p-rogue", "最近傍 = ローグ好き");
  assert.ok(hits[0].similarity > 0, "cosine > 0");

  // candidates 明示も可
  const all = listPoolPersonas();
  const hits2 = selectByAffinity(v, 2, all);
  assert.equal(hits2.length, 2, "k=2 (candidates 明示)");

  // FlowPersona 変換
  const fp = toFlowPersona(hits[0].persona, { role: "opinion", defaultModel: "claude-haiku-4-5-20251001", isLocal: false });
  assert.equal(fp.id, "p-rogue");
  assert.equal(fp.model, "claude-haiku-4-5-20251001", "model fallback");
  console.log("  [ok] 憑依: selectByAffinity + toFlowPersona");
}

// ── 憑依: テーマ類推 (B) ─────────────────────────────────────────
{
  // テーマと同義のテキストで作ったベクトルのペルソナが、テーマ類推で選ばれる
  const themed = textToVector("高難度を上達する達成感");
  insertPoolPersona({
    id: "p-themed",
    name: "達成感おじさん",
    role: "opinion",
    speechStyle: "熱血",
    traits: ["達成感"],
    affectVector: themed,
    origin: "generated",
    parentIds: [],
  });
  const hit = selectPossessionByTheme("高難度を上達する達成感について", 1)[0];
  assert.ok(hit, "テーマ類推でヒット");
  assert.equal(hit.persona.id, "p-themed", "テーマ最近傍 = 達成感おじさん");
  console.log("  [ok] 憑依: selectPossessionByTheme (テーマ類推)");
}

// ── findPoolPersona (G 相手解決 / F 親解決) ──────────────────────
{
  const { findPoolPersona } = await import("../../src/flow/persona-pool.js");
  assert.equal(findPoolPersona("p-rogue")?.id, "p-rogue", "id 一致");
  assert.equal(findPoolPersona("ローグ好き太郎")?.id, "p-rogue", "name 一致");
  assert.equal(findPoolPersona("存在しない"), null, "未解決は null");
  console.log("  [ok] findPoolPersona: id/name 解決");
}

// ── F: 合成 (averageVectors + synthesizePersona, mock LLM) ───────
{
  const { averageVectors, synthesizePersona } = await import("../../src/flow/persona-synthesize.js");
  const a = new Array(DIM as number).fill(0);
  a[0] = 1;
  const b = new Array(DIM as number).fill(0);
  b[0] = 0;
  b[1] = 1;
  const avg = averageVectors([a, b]);
  assert.equal(avg[0], 0.5, "平均[0]");
  assert.equal(avg[1], 0.5, "平均[1]");

  const mockLlm = {
    async invoke() {
      return {
        ok: true as const,
        text: '{"name":"融合ザムザ","speechStyle":"二面性","traits":["高難度志向","収集欲"]}',
      };
    },
  };
  const syn = await synthesizePersona({ parentRefs: ["p-rogue", "ソシャゲ花子"], llm: mockLlm });
  assert.equal(syn.origin, "synthesized", "origin=synthesized");
  assert.equal(syn.name, "融合ザムザ", "LLM 由来 name");
  assert.deepEqual(syn.parentIds.sort(), ["p-gacha2", "p-rogue"].sort(), "parentIds");
  assert.equal(syn.affectVector.length, DIM as number, "affect dim");
  // 保存され、プールから引ける
  assert.ok(getPoolPersona(syn.id), "合成ペルソナがプールに保存される");
  console.log("  [ok] F 合成: averageVectors + synthesizePersona (mock LLM)");

  // 親 2 体未満はエラー
  await assert.rejects(
    () => synthesizePersona({ parentRefs: ["p-rogue"], llm: mockLlm }),
    /親が 2 体以上/,
    "親不足はエラー"
  );
}

// ── C1 実在ユーザ採用 (evaluateSpeakers + adoptPersonas) ─────────
{
  const { evaluateSpeakers, adoptPersonas } = await import("../../src/flow/persona-adopt.js");
  const mk = (n: number, text: string, game: string) =>
    Array.from({ length: n }, () => ({ text, gameSlug: game }));
  const speakers = [
    // mixed polarity + 2 games + gap → 採用
    {
      speakerId: "ext:steam:good",
      source: "steam",
      opinions: [...mk(6, "great fun awesome", "game-a"), ...mk(6, "boring bad terrible", "game-b")],
    },
    // 全ポジ (2 ゲームとも好評) → all-positive 除外
    {
      speakerId: "ext:steam:allpos",
      source: "steam",
      opinions: [...mk(5, "great awesome best", "game-a"), ...mk(5, "fun amazing love", "game-b")],
    },
    // 意見不足 → 除外
    { speakerId: "ext:steam:few", source: "steam", opinions: mk(5, "good", "game-a") },
    // mixed だが 1 ゲーム → gap なし除外
    {
      speakerId: "ext:steam:onegame",
      source: "steam",
      opinions: [...mk(5, "great fun", "game-a"), ...mk(5, "bad boring", "game-a")],
    },
  ];

  const ev = evaluateSpeakers(speakers, { minOpinions: 10 });
  assert.deepEqual(
    ev.candidates.map((c) => c.speakerId),
    ["ext:steam:good"],
    "採用候補は good のみ"
  );
  const reasons = Object.fromEntries(ev.rejected.map((r) => [r.speakerId, r.reason]));
  assert.equal(reasons["ext:steam:allpos"], "all-positive");
  assert.equal(reasons["ext:steam:few"], "too-few");
  assert.equal(reasons["ext:steam:onegame"], "no-game-gap");
  // #125 追加特徴量: 賛否混在 (good) は polarity_bias 低め、ゲーム間ばらつき (dispersion) > 0。
  const good = ev.candidates[0];
  assert.ok(good.polarityBias >= 0 && good.polarityBias <= 1, "polarity_bias は 0..1");
  assert.ok(good.affectDispersion > 0, "2 ゲームで affect_dispersion > 0");
  console.log("  [ok] C1 evaluateSpeakers: ≥10/全ポジ除外/不足/gap なし + 追加特徴量");

  const res = adoptPersonas(speakers, { minOpinions: 10 });
  assert.equal(res.adopted.length, 1, "採用 1 名");
  assert.ok(res.adopted[0].name.startsWith("論者#"), "露出名は論者#");
  assert.equal(res.adopted[0].origin, "adopted", "origin=adopted");
  assert.equal(res.adopted[0].sourceSpeakerId, "ext:steam:good", "speaker アンカー");
  assert.ok(typeof res.adopted[0].typicality === "number", "typicality 記録");
  assert.equal(listPoolPersonas({ origin: "adopted" }).length, 1, "プールに 1 名");

  // upsert 冪等: 再実行で別個体を量産しない
  adoptPersonas(speakers, { minOpinions: 10 });
  assert.equal(listPoolPersonas({ origin: "adopted" }).length, 1, "再採用しても 1 名 (upsert)");
  console.log("  [ok] C1 adoptPersonas: 採用 + 論者#/typicality + speaker upsert 冪等");
}

console.log("persona-pool tests: all passed");
