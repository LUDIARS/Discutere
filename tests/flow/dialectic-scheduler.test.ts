/**
 * dialectic スケジューラ (respec 06 §3 / PR-B) テスト — 決定性。
 * - ① 未応答 rebut があるとき被反論者が指名される (決定的)
 * - ② 攻撃番: 接続数最大の Position を反対陣営の発言数最少ペルソナが攻める
 * - ③ 自由ターン: 発言数最少 (同点は入力順)
 * - 同一入力 → 同一出力 (指名の決定性)
 */

import assert from "node:assert/strict";

const { ArgumentGraph } = await import("../../src/flow/argument-graph.js");
const { scheduleNextTurn, nominationFallbackText, RESPOND_ACTS, ATTACK_ACTS, FREE_ACTS } =
  await import("../../src/flow/dialectic/scheduler.js");

function makePersona(id: string, name: string, role: "debater" | "opinion", stance: "pro" | "con" | "opinion") {
  return {
    id,
    name,
    role,
    stance,
    speechStyle: "",
    traits: [],
    model: "m",
    isLocal: false,
  };
}

const pro = makePersona("p-pro", "翔", "debater", "pro");
const con = makePersona("p-con", "凛", "debater", "con");
const op = makePersona("p-op", "葵", "opinion", "opinion");
const speakers = [pro, con, op];

// ── ① 未応答 rebut → 被反論者の応答権指名 ─────────────────────────────────────

{
  const graph = new ArgumentGraph();
  graph.apply({ id: "c-pro", personaId: "p-pro", act: "claim", targetId: null, turn: 1 });
  graph.apply({ id: "c-con", personaId: "p-con", act: "claim", targetId: null, turn: 2 });
  graph.apply({ id: "r1", personaId: "p-con", act: "rebut", targetId: "c-pro", turn: 3 });

  const anchors = [
    { personaId: "p-pro", stance: "pro" as const, utteranceId: "c-pro" },
    { personaId: "p-con", stance: "con" as const, utteranceId: "c-con" },
  ];
  const decision = scheduleNextTurn({ graph, speakers, positionAnchors: anchors, speakCounts: new Map() });
  assert.ok(decision, "decision が返る");
  assert.equal(decision!.kind, "respond", "未応答 rebut → 応答権指名");
  assert.equal(decision!.personaId, "p-pro", "被反論者 (claim の持ち主) が指名される");
  assert.equal(decision!.targetUtteranceId, "r1", "ターゲットは rebut 発話");
  assert.deepEqual([...decision!.allowedActs], [...RESPOND_ACTS], "acts は応答系に制限");

  // 決定性: 同一入力で同一出力。
  const again = scheduleNextTurn({ graph, speakers, positionAnchors: anchors, speakCounts: new Map() });
  assert.deepEqual(again, decision, "同一入力 → 同一指名 (決定的)");
  console.log("  [ok] scheduler: 未応答 rebut → 被反論者の応答権指名 (決定的)");
}

// ── ② 攻撃番: 接続数最大の Position を反対陣営が攻める ────────────────────────

{
  const graph = new ArgumentGraph();
  graph.apply({ id: "c-pro", personaId: "p-pro", act: "claim", targetId: null, turn: 1 });
  graph.apply({ id: "c-con", personaId: "p-con", act: "claim", targetId: null, turn: 2 });
  // con の Position に rebut 1 件 (defended 済 = 未応答なし) → 接続数最大は c-con。
  graph.apply({ id: "r1", personaId: "p-pro", act: "rebut", targetId: "c-con", turn: 3 });
  graph.apply({ id: "s1", personaId: "p-con", act: "support", targetId: "r1", turn: 4 });

  const anchors = [
    { personaId: "p-pro", stance: "pro" as const, utteranceId: "c-pro" },
    { personaId: "p-con", stance: "con" as const, utteranceId: "c-con" },
  ];
  const decision = scheduleNextTurn({ graph, speakers, positionAnchors: anchors, speakCounts: new Map() });
  assert.ok(decision);
  assert.equal(decision!.kind, "attack", "未応答なし → 攻撃番");
  assert.equal(decision!.targetUtteranceId, "c-con", "接続数最大の Position が焦点");
  assert.equal(decision!.personaId, "p-pro", "反対陣営 (pro) の debater が攻撃");
  assert.deepEqual([...decision!.allowedActs], [...ATTACK_ACTS], "acts は攻撃系に制限");
  console.log("  [ok] scheduler: 攻撃番 = 接続数最大の Position × 反対陣営");
}

// ── ② 同点の焦点は anchor 入力順 (決定的タイブレーク) ─────────────────────────

{
  const graph = new ArgumentGraph();
  graph.apply({ id: "c-pro", personaId: "p-pro", act: "claim", targetId: null, turn: 1 });
  graph.apply({ id: "c-con", personaId: "p-con", act: "claim", targetId: null, turn: 2 });

  const anchors = [
    { personaId: "p-pro", stance: "pro" as const, utteranceId: "c-pro" },
    { personaId: "p-con", stance: "con" as const, utteranceId: "c-con" },
  ];
  const decision = scheduleNextTurn({ graph, speakers, positionAnchors: anchors, speakCounts: new Map() });
  assert.ok(decision);
  assert.equal(decision!.kind, "attack");
  assert.equal(decision!.targetUtteranceId, "c-pro", "接続数同点は anchor 先頭 (入力順)");
  assert.equal(decision!.personaId, "p-con", "pro の Position は con が攻める");
  console.log("  [ok] scheduler: 同点タイブレークは入力順で決定的");
}

// ── ③ 自由ターン: Position が無ければ発言数最少ペルソナ ───────────────────────

{
  const graph = new ArgumentGraph();
  const speakCounts = new Map([
    ["p-pro", 3],
    ["p-con", 2],
    ["p-op", 1],
  ]);
  const decision = scheduleNextTurn({ graph, speakers, positionAnchors: [], speakCounts });
  assert.ok(decision);
  assert.equal(decision!.kind, "free", "アンカーなし → 自由ターン");
  assert.equal(decision!.personaId, "p-op", "発言数最少ペルソナが指名される");
  assert.deepEqual([...decision!.allowedActs], [...FREE_ACTS], "acts は自由 (synthesize 以外)");

  // 同点は speakers 入力順。
  const tie = scheduleNextTurn({
    graph,
    speakers,
    positionAnchors: [],
    speakCounts: new Map(),
  });
  assert.equal(tie!.personaId, "p-pro", "同点は入力順の先頭");
  console.log("  [ok] scheduler: 自由ターン = 発言数最少 (同点は入力順)");
}

// ── 発言者ゼロ → null ─────────────────────────────────────────────────────────

{
  const graph = new ArgumentGraph();
  const decision = scheduleNextTurn({ graph, speakers: [], positionAnchors: [], speakCounts: new Map() });
  assert.equal(decision, null, "発言者ゼロ → null");
  console.log("  [ok] scheduler: 発言者ゼロ → null");
}

// ── ファシリテーターの fallback 文 (決定的レンダリング) ───────────────────────

{
  const text = nominationFallbackText(
    {
      kind: "respond",
      personaId: "p-pro",
      targetUtteranceId: "r1",
      allowedActs: RESPOND_ACTS,
      reason: "",
    },
    "翔",
    { id: "r1", personaName: "凛", text: "ARPU が下がるはず" }
  );
  assert.ok(text.includes("凛"), "反論者名が入る");
  assert.ok(text.includes("翔"), "被指名者名が入る");
  console.log("  [ok] scheduler: nominationFallbackText (決定的)");
}

console.log("dialectic-scheduler tests: all passed");
