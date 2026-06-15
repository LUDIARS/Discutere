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

console.log("persona-pool tests: all passed");
