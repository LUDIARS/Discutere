/**
 * 論拠の応酬 (dialectic.md §2.6) テスト — 決定性。
 * - 応答順は幅優先 (定立の論拠すべて → その反論すべて → ...)
 * - 反論は論拠 1 件のノードとして親にぶら下がり、相手側が答える
 * - 決着: 成り立った反論があれば崩れる / 再反論が成り立てば守り切る / 打ち切りは決着つかず
 * - 根拠状態への写像 (unchallenged / defended / conceded / challenged)
 * - 深さ上限・ターン上限の打ち切り
 * - 応答パース (rebut / concede / JSON 崩れ = question)
 * - 論証グラフ: 反論への再反論で元の反論が応答済みになる
 */

import assert from "node:assert/strict";

const { GroundExchange, opposingSide } = await import("../../src/flow/dialectic/exchange.js");
const { parseExchangeResponse, buildExchangePrompt } = await import("../../src/flow/dialectic/exchange-prompt.js");
const { pickSideSpeaker } = await import("../../src/flow/dialectic/exchange-runner.js");
const { ArgumentGraph } = await import("../../src/flow/argument-graph.js");

type Side = "pro" | "con";

function opening(side: Side, n: number) {
  return {
    utteranceId: `u-${side}-${n}`,
    side,
    personaId: `p-${side}`,
    groundId: `${side}-G${n}`,
    data: `${side} データ ${n}`,
    warrant: `${side} 論拠 ${n}`,
    text: `${side} の論拠 ${n}`,
  };
}

function rebuttal(side: Side, tag: string) {
  return {
    utteranceId: `u-${tag}`,
    side,
    personaId: `p-${side}`,
    groundId: null,
    data: `反論データ ${tag}`,
    warrant: `反論論拠 ${tag}`,
    text: `反論 ${tag}`,
  };
}

// ── 幅優先: 定立の論拠を全部さばいてから反論へ ────────────────────────────────
{
  const ex = new GroundExchange({ maxDepth: 4 });
  const p1 = ex.addOpening(opening("pro", 1));
  const c1 = ex.addOpening(opening("con", 1));
  const p2 = ex.addOpening(opening("pro", 2));

  assert.equal(ex.next()?.id, p1.id);
  const r1 = ex.rebut(p1.id, rebuttal("con", "r1"));
  assert.equal(r1.depth, 1);
  assert.equal(r1.parentId, p1.id);
  assert.equal(ex.next()?.id, c1.id, "反論 r1 より先に次の定立の論拠に答える");
  ex.accept(c1.id);
  assert.equal(ex.next()?.id, p2.id);
  ex.fail(p2.id);
  assert.equal(ex.next()?.id, r1.id, "定立の論拠が尽きてから反論への応答");
  assert.equal(opposingSide(r1.side), "pro");
  const rr = ex.rebut(r1.id, rebuttal("pro", "rr1"));
  assert.equal(ex.next()?.id, rr.id);
  ex.accept(rr.id); // 反論側 (con) が再反論を認めた
  assert.equal(ex.settled(), true);

  // p1: 反論 r1 は再反論 rr で崩れた → p1 は守り切った
  assert.equal(ex.verdict(rr.id), "standing");
  assert.equal(ex.verdict(r1.id), "refuted");
  assert.equal(ex.verdict(p1.id), "standing");
  const states = ex.groundStates();
  assert.equal(states.get("pro-G1"), "defended");
  assert.equal(states.get("con-G1"), "unchallenged", "認められた論拠は反論なし扱い");
  assert.equal(states.get("pro-G2"), "unchallenged", "有効な反論にならなかった論拠は残る");
  assert.deepEqual(ex.path(rr.id).map((n) => n.id), [p1.id, r1.id, rr.id]);
  assert.deepEqual(ex.standingCount("pro"), { standing: 2, total: 2 });
}

// ── 崩れ方: 持ち主が反論を認めた = conceded / 再反論が崩れた = challenged ─────
{
  const ex = new GroundExchange({ maxDepth: 4 });
  const p1 = ex.addOpening(opening("pro", 1));
  const c1 = ex.addOpening(opening("con", 1));
  const r1 = ex.rebut(p1.id, rebuttal("con", "r1"));
  const r2 = ex.rebut(c1.id, rebuttal("pro", "r2"));
  ex.accept(r1.id); // pro が r1 を認めた
  const rr2 = ex.rebut(r2.id, rebuttal("con", "rr2")); // con が r2 に再反論
  const rrr2 = ex.rebut(rr2.id, rebuttal("pro", "rrr2")); // pro が再々反論
  ex.fail(rrr2.id); // con は有効に返せなかった
  assert.equal(ex.settled(), true);

  const states = ex.groundStates();
  assert.equal(states.get("pro-G1"), "conceded");
  // rrr2 成立 → rr2 崩れ → r2 成立 → c1 崩れ (con は認めていないので challenged)
  assert.equal(ex.verdict(c1.id), "refuted");
  assert.equal(states.get("con-G1"), "challenged");
  assert.deepEqual(ex.standingCount("con"), { standing: 0, total: 1 });
}

// ── 深さ上限: その深さの反論には答えさせず、決着つかず (根拠は崩れない) ─────
{
  const ex = new GroundExchange({ maxDepth: 2 });
  const p1 = ex.addOpening(opening("pro", 1));
  const r1 = ex.rebut(p1.id, rebuttal("con", "r1"));
  const rr1 = ex.rebut(r1.id, rebuttal("pro", "rr1"));
  assert.equal(rr1.status, "cut");
  assert.equal(ex.settled(), true);
  assert.equal(ex.verdict(rr1.id), "contested");
  assert.equal(ex.verdict(r1.id), "contested");
  assert.equal(ex.verdict(p1.id), "contested");
  assert.equal(ex.groundStates().get("pro-G1"), "defended", "最後に答えた側を勝たせない");
}

// ── ターン上限: 残りを打ち切る。未攻撃の論拠は unchallenged ─────────────────────
{
  const ex = new GroundExchange({ maxDepth: 4 });
  const p1 = ex.addOpening(opening("pro", 1));
  ex.addOpening(opening("con", 1));
  ex.rebut(p1.id, rebuttal("con", "r1"));
  assert.equal(ex.cutRemaining(), 2, "con-G1 と r1 が打ち切られる");
  assert.equal(ex.settled(), true);
  const states = ex.groundStates();
  assert.equal(states.get("pro-G1"), "defended", "反論 r1 は打ち切りで決着つかず");
  assert.equal(states.get("con-G1"), "unchallenged");
}

// ── 応答パース ────────────────────────────────────────────────────────────────
{
  const r = parseExchangeResponse(
    JSON.stringify({ act: "rebut", attack: "warrant", kind: "induction", data: "D", warrant: "W", text: "違うと思う" })
  );
  assert.equal(r.act, "rebut");
  assert.equal(r.attack, "warrant");
  assert.equal(r.kind, "induction");
  assert.equal(r.data, "D");
  assert.equal(r.warrant, "W");

  const c = parseExchangeResponse('{"act":"concede","text":"それは認める"}');
  assert.equal(c.act, "concede");

  const warns: string[] = [];
  const q = parseExchangeResponse("JSON じゃない返事", (m) => warns.push(m));
  assert.equal(q.act, "question");
  assert.equal(q.degraded, true);
  assert.equal(q.text, "JSON じゃない返事");
  assert.equal(warns.length, 1);
}

// ── 応答プロンプト: 応酬の経路と「論拠 1 つだけ」を指示する ────────────────────
{
  const ex = new GroundExchange({ maxDepth: 4 });
  const p1 = ex.addOpening(opening("pro", 1));
  const r1 = ex.rebut(p1.id, rebuttal("con", "r1"));
  const persona = { id: "p-pro", name: "翔", role: "debater", stance: "pro", speechStyle: "", traits: [], model: "m", isLocal: false };
  const prompt = buildExchangePrompt({
    persona: persona as never,
    stance: "pro",
    issue: { id: "i", sessionId: "s", title: "論点", ordinal: 1, source: "llm", status: "open" },
    positions: [],
    personaNames: new Map([["p-pro", "翔"], ["p-con", "凛"]]),
    path: ex.path(r1.id),
  });
  assert.match(prompt, /論拠 \[pro-G1\] 翔 \(あなた\)/);
  assert.match(prompt, /反論 1 凛/);
  assert.match(prompt, /凛さんの最後の反論に答えて/);
  assert.match(prompt, /論拠 1 つだけ/);
  assert.match(prompt, /rebut \| concede/);
}

// ── 陣営の発言者: 同じ立場の debater のうち発言数最少・同点は入力順 (意見役は陣営に入らない) ──
{
  const mk = (id: string, stance: string, role = "debater") =>
    ({ id, name: id, role, stance, speechStyle: "", traits: [], model: "m", isLocal: false }) as never;
  const speakers = [mk("a", "pro"), mk("b", "con"), mk("c", "pro"), mk("o", "pro", "opinion")];
  assert.equal(pickSideSpeaker(speakers, "pro", new Map())?.id, "a");
  assert.equal(pickSideSpeaker(speakers, "pro", new Map([["a", 2], ["c", 1]]))?.id, "c");
  assert.equal(pickSideSpeaker(speakers, "pro", new Map([["a", 2], ["c", 2]]))?.id, "a", "意見役は選ばれない");
  assert.equal(pickSideSpeaker(speakers, "con", new Map())?.id, "b");
}

// ── 論証グラフ: 反論への再反論で元の反論が応答済み、新しい反論の応答義務は元の反論者 ─
{
  const g = new ArgumentGraph();
  g.apply({ id: "g1", personaId: "p-pro", act: "claim", targetId: null, turn: 1 });
  g.apply({ id: "r1", personaId: "p-con", act: "rebut", targetId: "g1", turn: 2 });
  g.apply({ id: "rr1", personaId: "p-pro", act: "rebut", targetId: "r1", turn: 3 });
  assert.equal(g.rebuttalById("r1")?.state, "defended");
  const rr = g.rebuttalById("rr1");
  assert.equal(rr?.state, "challenged");
  assert.equal(rr?.targetPersonaId, "p-con");
  assert.deepEqual(g.unresolvedRebuttals().map((e) => e.rebutId), ["rr1"]);
}

console.log("dialectic-exchange: all passed");
