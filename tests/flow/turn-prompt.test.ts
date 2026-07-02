/**
 * dialectic ターンプロンプト + パース (respec 05 / PR-B) テスト。
 * - プロンプトに 論点 / Position ダイジェスト / 未応答反論 / 許可 act が載る
 * - JSON 応答のパース (act/target/groundId/text)
 * - JSON 崩れ応答 → act=claim で全文受理 + warn (発話が落ちない)
 * - 許可外 act の矯正 / 不明 target の指名ターゲットへのフォールバック
 */

import assert from "node:assert/strict";

const { buildDialecticTurnPrompt, parseTurnResponse } = await import(
  "../../src/flow/dialectic/turn-prompt.js"
);

const persona = {
  id: "p-con",
  name: "凛",
  role: "debater" as const,
  stance: "con" as const,
  speechStyle: "芯のある主張口調",
  traits: ["論点を掴む"],
  model: "m",
  isLocal: false,
  valueAxis: "プレイヤー体験を最優先",
};

const issue = {
  id: "i1",
  sessionId: "s1",
  title: "ガチャ緩和は長期的に売上を伸ばすか?",
  ordinal: 1,
  source: "llm" as const,
  status: "open" as const,
};

const positions = [
  {
    id: "pos-pro",
    issueId: "i1",
    personaId: "p-pro",
    stance: "pro" as const,
    claim: "緩和は継続率を上げ長期売上に効く",
    grounds: [
      { id: "pro-G1", text: "離脱理由の 1 位が天井の高さ", state: "unchallenged" as const },
      { id: "pro-G2", text: "緩和後に DAU が伸びた事例", state: "challenged" as const },
    ],
    values: ["体験 > 短期収益"],
  },
  {
    id: "pos-con",
    issueId: "i1",
    personaId: "p-con",
    stance: "con" as const,
    claim: "緩和は ARPU を直撃し売上を下げる",
    grounds: [{ id: "con-G1", text: "重課金層の支出が下がる", state: "unchallenged" as const }],
    values: ["収益 > 体験"],
  },
];

// ── プロンプト組み立て ─────────────────────────────────────────────────────────

{
  const utterances = new Map([
    ["u1", { id: "u1", personaName: "翔", text: "緩和は継続率を上げると思う" }],
    ["u2", { id: "u2", personaName: "凛", text: "ARPU が下がるよ" }],
  ]);
  const prompt = buildDialecticTurnPrompt({
    persona,
    stance: "con",
    issue,
    positions,
    unresolvedRebuttals: [
      { rebutId: "u2", targetId: "u1", byPersonaId: "p-con", targetPersonaId: "p-pro", state: "challenged" },
    ],
    utterances,
    personaNames: new Map([
      ["p-pro", "翔"],
      ["p-con", "凛"],
    ]),
    allowedActs: ["rebut", "question"],
    targetUtteranceId: "u1",
  });

  assert.ok(prompt.includes(issue.title), "論点タイトルが載る");
  assert.ok(prompt.includes("[pro-G1]"), "根拠 id 付きダイジェストが載る");
  assert.ok(prompt.includes("(challenged)"), "根拠の状態が載る");
  assert.ok(prompt.includes("凛 (あなた)"), "自分の Position が「あなた」表示");
  assert.ok(prompt.includes("rebut | question"), "許可 act が明示される");
  assert.ok(!prompt.includes('"act": "<claim'), "許可外 act は選択肢に出ない");
  assert.ok(prompt.includes("# 応答対象"), "応答対象ブロックが載る");
  assert.ok(prompt.includes("緩和は継続率を上げると思う"), "ターゲット本文が載る");
  assert.ok(prompt.includes("# 未応答の反論"), "未応答反論の節が載る");
  console.log("  [ok] buildDialecticTurnPrompt: ダイジェスト + 許可 act + 応答対象");
}

// ── 正常パース ────────────────────────────────────────────────────────────────

{
  const parsed = parseTurnResponse(
    '{"act": "rebut", "target": "u1", "groundId": "pro-G1", "text": "その事例は条件が違うよ"}',
    { allowedActs: ["rebut", "question"], validTargets: new Set(["u1", "u2"]) }
  );
  assert.equal(parsed.act, "rebut");
  assert.equal(parsed.targetId, "u1");
  assert.equal(parsed.groundId, "pro-G1");
  assert.equal(parsed.text, "その事例は条件が違うよ");
  assert.equal(parsed.degraded, false);
  console.log("  [ok] parseTurnResponse: 正常 JSON");
}

// ── 前置き / コードフェンス混入でもパースできる ────────────────────────────────

{
  const parsed = parseTurnResponse(
    '了解です。\n```json\n{"act": "concede", "target": "u2", "text": "確かにそこは認める"}\n```',
    { allowedActs: ["support", "concede", "rebut", "question"], validTargets: new Set(["u2"]) }
  );
  assert.equal(parsed.act, "concede");
  assert.equal(parsed.targetId, "u2");
  assert.equal(parsed.degraded, false);
  console.log("  [ok] parseTurnResponse: コードフェンス混入の救済");
}

// ── JSON 崩れ → act=claim で全文受理 + warn (発話を捨てない) ───────────────────

{
  const warnings: string[] = [];
  const raw = "JSON じゃなくて普通に喋っちゃうけど、緩和はやっぱり慎重にすべきだと思う。";
  const parsed = parseTurnResponse(raw, {
    allowedActs: ["rebut", "question"],
    validTargets: new Set(["u1"]),
    warn: (m) => warnings.push(m),
  });
  assert.equal(parsed.act, "claim", "JSON 崩れ → act=claim");
  assert.equal(parsed.targetId, null, "target=null");
  assert.equal(parsed.text, raw, "全文が発話として残る (捨てない)");
  assert.equal(parsed.degraded, true, "degrade フラグが立つ");
  assert.ok(warnings.some((w) => w.includes("パース失敗")), "warn が出る");
  console.log("  [ok] parseTurnResponse: JSON 崩れ → claim degrade + warn");
}

// ── 許可外 act は許可集合の先頭に矯正 ─────────────────────────────────────────

{
  const warnings: string[] = [];
  const parsed = parseTurnResponse('{"act": "claim", "target": null, "text": "新しい話をしたい"}', {
    allowedActs: ["support", "concede", "rebut", "question"],
    validTargets: new Set(["u2"]),
    defaultTargetId: "u2",
    warn: (m) => warnings.push(m),
  });
  assert.equal(parsed.act, "support", "許可外 claim → 先頭の support に矯正");
  assert.equal(parsed.targetId, "u2", "応答系 act は指名ターゲットへフォールバック");
  assert.ok(warnings.some((w) => w.includes("許可外")), "矯正の warn が出る");
  console.log("  [ok] parseTurnResponse: 許可外 act の矯正 + 指名ターゲット");
}

// ── 不明 target → 指名ターゲットに置換 ────────────────────────────────────────

{
  const warnings: string[] = [];
  const parsed = parseTurnResponse(
    '{"act": "rebut", "target": "no-such-id", "text": "それは違う"}',
    {
      allowedActs: ["rebut", "question"],
      validTargets: new Set(["u1"]),
      defaultTargetId: "u1",
      warn: (m) => warnings.push(m),
    }
  );
  assert.equal(parsed.targetId, "u1", "不明 target → 指名ターゲット");
  assert.ok(warnings.some((w) => w.includes("不明な utterance id")), "warn が出る");
  console.log("  [ok] parseTurnResponse: 不明 target のフォールバック");
}

console.log("turn-prompt tests: all passed");
