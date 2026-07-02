/**
 * 論証グラフ (respec 05 / PR-B) テスト。
 * - 遷移: rebut→challenged / support(防御)→defended / concede→conceded が決定的
 * - unresolvedRebuttals / unansweredClaims / newClaimsSince が手作りシーケンスで期待値どおり
 * - coerceAct: act=null (旧行) は claim 扱い / buildGraphFromRows の後方互換
 */

import assert from "node:assert/strict";

const { ArgumentGraph, coerceAct, buildGraphFromRows } = await import(
  "../../src/flow/argument-graph.js"
);

// ── coerceAct: 旧行 (null) / 未知値は claim 扱い ────────────────────────────────

{
  assert.equal(coerceAct(null), "claim", "act=null → claim (旧行の後方互換)");
  assert.equal(coerceAct(undefined), "claim", "act=undefined → claim");
  assert.equal(coerceAct("garbage"), "claim", "未知 act → claim");
  assert.equal(coerceAct("rebut"), "rebut", "既知 act はそのまま");
  assert.equal(coerceAct("synthesize"), "synthesize", "synthesize もそのまま");
  console.log("  [ok] coerceAct: null/未知 → claim");
}

// ── 遷移: rebut → challenged ────────────────────────────────────────────────────

{
  const g = new ArgumentGraph();
  g.apply({ id: "c1", personaId: "pro", act: "claim", targetId: null, turn: 1 });
  g.apply({ id: "r1", personaId: "con", act: "rebut", targetId: "c1", turn: 2 });

  const unresolved = g.unresolvedRebuttals();
  assert.equal(unresolved.length, 1, "rebut → 未応答反論 1 件");
  assert.equal(unresolved[0].rebutId, "r1");
  assert.equal(unresolved[0].targetId, "c1");
  assert.equal(unresolved[0].targetPersonaId, "pro", "被反論者 = claim の持ち主");
  assert.equal(unresolved[0].state, "challenged");
  console.log("  [ok] ArgumentGraph: rebut → challenged");
}

// ── 遷移: support (被反論側の防御) → defended ──────────────────────────────────

{
  const g = new ArgumentGraph();
  g.apply({ id: "c1", personaId: "pro", act: "claim", targetId: null, turn: 1 });
  g.apply({ id: "r1", personaId: "con", act: "rebut", targetId: "c1", turn: 2 });
  g.apply({ id: "s1", personaId: "pro", act: "support", targetId: "r1", turn: 3 });

  assert.equal(g.unresolvedRebuttals().length, 0, "防御成功 → 未応答反論 0");
  assert.equal(g.rebuttalById("r1")?.state, "defended", "辺の状態 = defended");
  assert.equal(g.concessions().length, 0, "concede なし");
  console.log("  [ok] ArgumentGraph: support (防御) → defended");
}

// ── 反論者自身の support は防御にならない ──────────────────────────────────────

{
  const g = new ArgumentGraph();
  g.apply({ id: "c1", personaId: "pro", act: "claim", targetId: null, turn: 1 });
  g.apply({ id: "r1", personaId: "con", act: "rebut", targetId: "c1", turn: 2 });
  g.apply({ id: "s1", personaId: "con", act: "support", targetId: "r1", turn: 3 });

  assert.equal(g.unresolvedRebuttals().length, 1, "自分の反論への support は防御ではない");
  console.log("  [ok] ArgumentGraph: 反論者自身の support は防御にならない");
}

// ── 遷移: concede → conceded (止揚の原材料) ────────────────────────────────────

{
  const g = new ArgumentGraph();
  g.apply({ id: "c1", personaId: "pro", act: "claim", targetId: null, turn: 1 });
  g.apply({ id: "r1", personaId: "con", act: "rebut", targetId: "c1", turn: 2 });
  g.apply({ id: "x1", personaId: "pro", act: "concede", targetId: "r1", turn: 3 });

  assert.equal(g.unresolvedRebuttals().length, 0, "concede → 未応答反論 0");
  const concessions = g.concessions();
  assert.equal(concessions.length, 1, "concession 1 件");
  assert.equal(concessions[0].rebutId, "r1");
  assert.equal(concessions[0].targetId, "c1");
  console.log("  [ok] ArgumentGraph: concede → conceded");
}

// ── claim を直接 concede → その claim への未応答 rebut を一括 conceded ─────────

{
  const g = new ArgumentGraph();
  g.apply({ id: "c1", personaId: "pro", act: "claim", targetId: null, turn: 1 });
  g.apply({ id: "r1", personaId: "con", act: "rebut", targetId: "c1", turn: 2 });
  g.apply({ id: "r2", personaId: "con", act: "rebut", targetId: "c1", turn: 3 });
  g.apply({ id: "x1", personaId: "pro", act: "concede", targetId: "c1", turn: 4 });

  assert.equal(g.unresolvedRebuttals().length, 0, "claim 全体の譲歩で未応答反論 0");
  assert.equal(g.concessions().length, 2, "両方の rebut が conceded");
  console.log("  [ok] ArgumentGraph: claim 直接 concede → 未応答 rebut 一括 conceded");
}

// ── unansweredClaims / newClaimsSince ─────────────────────────────────────────

{
  const g = new ArgumentGraph();
  g.apply({ id: "c1", personaId: "pro", act: "claim", targetId: null, turn: 1 });
  g.apply({ id: "c2", personaId: "con", act: "claim", targetId: null, turn: 2 });
  g.apply({ id: "r1", personaId: "con", act: "rebut", targetId: "c1", turn: 3 });
  g.apply({ id: "c3", personaId: "op", act: "claim", targetId: null, turn: 7 });

  assert.deepEqual(g.unansweredClaims(), ["c2", "c3"], "触れられていない claim のみ");
  assert.equal(g.newClaimsSince(0), 3, "turn 0 より後の claim = 3");
  assert.equal(g.newClaimsSince(2), 1, "turn 2 より後の claim = c3 のみ");
  assert.equal(g.newClaimsSince(7), 0, "turn 7 より後の claim なし (枯渇)");
  console.log("  [ok] ArgumentGraph: unansweredClaims / newClaimsSince");
}

// ── edgeCountFor (スケジューラの接続数) ────────────────────────────────────────

{
  const g = new ArgumentGraph();
  g.apply({ id: "c1", personaId: "pro", act: "claim", targetId: null, turn: 1 });
  g.apply({ id: "c2", personaId: "con", act: "claim", targetId: null, turn: 2 });
  g.apply({ id: "r1", personaId: "con", act: "rebut", targetId: "c1", turn: 3 });
  g.apply({ id: "r2", personaId: "op", act: "rebut", targetId: "c1", turn: 4 });

  assert.equal(g.edgeCountFor("c1"), 2, "c1 への rebut 2 件");
  assert.equal(g.edgeCountFor("c2"), 0, "c2 への rebut なし");
  console.log("  [ok] ArgumentGraph: edgeCountFor");
}

// ── buildGraphFromRows: act=null 旧行の後方互換 (claim 扱いで壊れない) ──────────

{
  const g = buildGraphFromRows([
    { id: "u1", persona_id: "p1", act: null, target_id: null, turn: 1 },
    { id: "u2", persona_id: "p2", act: null, target_id: null, turn: 2 },
    { id: "u3", persona_id: "p2", act: "rebut", target_id: "u1", turn: 3 },
  ]);
  assert.equal(g.newClaimsSince(0), 2, "act=null 旧行は claim として数えられる");
  assert.equal(g.unresolvedRebuttals().length, 1, "新行の rebut は旧行 claim に張れる");
  assert.deepEqual(g.unansweredClaims(), ["u2"], "旧行 claim の未応答判定も動く");
  console.log("  [ok] buildGraphFromRows: act=null 旧行 → claim 扱いで再構築");
}

console.log("argument-graph tests: all passed");
