import assert from "node:assert/strict";

import {
  rrfFuse,
  mmrSelect,
  dedupeNearDuplicates,
} from "../../src/core/vectors/hybrid-rank.js";

// ── rrfFuse: 順位融合 ────────────────────────────────────────────────────────
{
  // 両系で上位の b が、片系 1 位ずつの a/c より高くなる。
  const fused = rrfFuse([
    ["a", "b", "c"],
    ["b", "a", "d"],
  ]);
  assert.ok(fused.get("b")! > fused.get("a")!, "両系上位の id が最上位");
  assert.ok(fused.get("a")! > fused.get("c")!, "2 系合算 > 片系のみ");
  assert.ok(fused.has("d"), "片系にしか無い id も残る");

  // 重み付け: keyword 系を 0 にすればベクトル系の順位が支配する。
  const weighted = rrfFuse(
    [
      ["a", "b"],
      ["b", "a"],
    ],
    [0, 1]
  );
  assert.ok(weighted.get("b")! > weighted.get("a")!, "重み 0 の系は寄与しない");
}
console.log("ok rrfFuse");

// ── mmrSelect: 関連度 × 多様性 ──────────────────────────────────────────────
{
  // ほぼ同一ベクトルの v1/v2 と直交する v3。関連度は v1 > v2 > v3 だが、
  // 2 件選ぶなら多様性から v1 + v3 が選ばれる。
  const picked = mmrSelect(
    [
      { id: "v1", relevance: 1.0, vector: [1, 0] },
      { id: "v2", relevance: 0.9, vector: [0.999, 0.01] },
      { id: "v3", relevance: 0.5, vector: [0, 1] },
    ],
    2,
    0.5
  );
  assert.deepEqual(
    picked.map((p) => p.id).sort(),
    ["v1", "v3"],
    "近接ベクトルより直交ベクトルを優先"
  );

  // ベクトル無し候補も relevance で競合し、部分索引で上位候補を落とさない。
  const mixed = mmrSelect(
    [
      { id: "v1", relevance: 1.0, vector: [1, 0] },
      { id: "n1", relevance: 0.8 },
      { id: "n2", relevance: 0.2 },
    ],
    2
  );
  assert.deepEqual(mixed.map((p) => p.id), ["v1", "n1"], "ベクトル無しも relevance 順で残る");
  const partialIndex = mmrSelect(
    [
      { id: "keyword-top", relevance: 1.0 },
      { id: "embedded-low", relevance: 0.8, vector: [1, 0] },
    ],
    1
  );
  assert.deepEqual(
    partialIndex.map((p) => p.id),
    ["keyword-top"],
    "部分索引でも relevance 上位のベクトル無し候補を選べる"
  );

  assert.deepEqual(mmrSelect([], 3), [], "空入力は空");
  assert.deepEqual(mmrSelect([{ id: "a", relevance: 1, vector: [1] }], 0), [], "k=0 は空");
}
console.log("ok mmrSelect");

// ── dedupeNearDuplicates: 近似重複の先勝ち畳み ───────────────────────────────
{
  const items = [
    { id: "a", content: "このゲームの引っ張りアクションが最高に気持ちいい" },
    { id: "b", content: "このゲームの引っ張りアクションが最高に気持ちいい!" }, // ほぼ同文
    { id: "c", content: "ガチャの排出率が渋すぎて課金する気になれない" },
  ];
  const kept = dedupeNearDuplicates(items);
  assert.deepEqual(kept.map((k) => k.id), ["a", "c"], "ほぼ同文は先勝ちで 1 件に畳む");

  // 閾値を上げれば畳まれない。
  assert.equal(dedupeNearDuplicates(items, 0.999).length, 3, "閾値 0.999 では全部残る");

  // 短文・空文でも落ちない。
  assert.equal(dedupeNearDuplicates([{ id: "x", content: "" }, { id: "y", content: "あ" }]).length, 2);
}
console.log("ok dedupeNearDuplicates");

console.log("hybrid-rank tests: all passed");
