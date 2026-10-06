/**
 * 前提ルール (dialectic.md §2.5) テスト。
 * - アンカー検査: 突く要素なし / 根拠 id 不明 / 譲歩済み根拠 → unanchored、限定は根拠 id 不要
 * - 判定 LLM: ラベル抽出、障害・不明ラベルは ok に倒す
 * - gateRebut: judge=false は判定 LLM を呼ばない
 * - 正規化: 論拠の種類・突く要素の英日ラベル
 */

import assert from "node:assert/strict";

const { checkRebutAnchor, parseRuleVerdict, judgeRuleViolation, gateRebut, buildRuleJudgePrompt } = await import(
  "../../src/flow/dialectic/rule-check.js"
);
const { coerceAttackPoint, coerceInferenceKind } = await import("../../src/flow/dialectic/premise-rules.js");

const issue = {
  id: "i1",
  sessionId: "s1",
  title: "ガチャ緩和は長期的に売上を伸ばすか?",
  ordinal: 1,
  source: "llm" as const,
  status: "open" as const,
};

const position = {
  id: "pos-pro",
  issueId: "i1",
  personaId: "p-pro",
  stance: "pro" as const,
  claim: "緩和は継続率を上げ長期売上に効く",
  qualifier: "天井で離脱するライト層",
  grounds: [
    { id: "pro-G1", text: "離脱理由の 1 位が天井の高さ", state: "unchallenged" as const, kind: "deduction" as const, warrant: "主因を緩めれば継続する" },
    { id: "pro-G2", text: "他ゲームで緩和後に DAU が伸びた", state: "conceded" as const, kind: "induction" as const },
  ],
  values: [],
};

function fakeLlm(response: { ok: true; text: string } | { ok: false; error: string }) {
  const calls: string[] = [];
  return {
    calls,
    async invoke(args: { prompt: string }) {
      calls.push(args.prompt);
      return response;
    },
  };
}

// ── アンカー検査 ──────────────────────────────────────────────────────────────

{
  const tw = { data: "コア層の離脱理由は天井ではない", warrant: "主因でなければ緩和しても継続しない" };
  assert.deepEqual(
    checkRebutAnchor({ position, attack: "warrant", groundId: "pro-G1", data: null, warrant: tw.warrant }),
    { ok: false, foul: "incomplete" },
    "反論自身のデータが無ければトゥールミンを満たさない"
  );
  assert.deepEqual(
    checkRebutAnchor({ position, attack: "warrant", groundId: "pro-G1", data: tw.data, warrant: null }),
    { ok: false, foul: "incomplete" },
    "反論自身の論拠が無ければトゥールミンを満たさない"
  );
  assert.deepEqual(checkRebutAnchor({ position, attack: null, groundId: "pro-G1", ...tw }), { ok: false, foul: "unanchored" });
  assert.deepEqual(checkRebutAnchor({ position, attack: "data", groundId: null, ...tw }), { ok: false, foul: "unanchored" });
  assert.deepEqual(checkRebutAnchor({ position, attack: "data", groundId: "pro-G9", ...tw }), { ok: false, foul: "unanchored" });
  assert.deepEqual(
    checkRebutAnchor({ position, attack: "warrant", groundId: "pro-G2", ...tw }),
    { ok: false, foul: "unanchored" },
    "譲歩済みの根拠は突けない"
  );
  const ok = checkRebutAnchor({ position, attack: "warrant", groundId: "pro-G1", ...tw });
  assert.ok(ok.ok && ok.ground?.id === "pro-G1", "トゥールミンを満たし未譲歩の根拠を指せば有効");
  const q = checkRebutAnchor({ position, attack: "qualifier", groundId: null, ...tw });
  assert.ok(q.ok && q.ground === null, "限定を突くなら根拠 id は不要");
  console.log("  [ok] checkRebutAnchor: 突く要素と根拠 id の宣言を検査");
}

// ── 判定 LLM ──────────────────────────────────────────────────────────────────

{
  assert.equal(parseRuleVerdict("out_of_scope"), "out_of_scope");
  assert.equal(parseRuleVerdict("ラベル: dismissal"), "dismissal");
  assert.equal(parseRuleVerdict("OK"), "ok");
  assert.equal(parseRuleVerdict("わからない"), null);

  const prompt = buildRuleJudgePrompt({
    issue,
    position,
    ground: position.grounds[0],
    attack: "warrant",
    rebutText: "コア層は離脱しないよ",
    rebutData: "コア層の離脱理由は天井ではない",
    rebutWarrant: "主因でなければ緩和しても継続しない",
  });
  assert.ok(prompt.includes("反論のデータ: コア層の離脱理由は天井ではない"), "反論のデータが判定材料に載る");
  assert.ok(prompt.includes("限定: 天井で離脱するライト層"), "限定が判定材料に載る");
  assert.ok(prompt.includes("論拠: 主因を緩めれば継続する"), "論拠が判定材料に載る");
  assert.ok(prompt.includes("「論拠」を突くと宣言"), "宣言した要素が載る");

  const args = {
    issue,
    position,
    ground: position.grounds[0],
    attack: "warrant" as const,
    rebutText: "x",
    rebutData: "d",
    rebutWarrant: "w",
  };
  assert.equal(await judgeRuleViolation({ ...args, llm: fakeLlm({ ok: true, text: "straw_man" }) as any }), "straw_man");

  const warnings: string[] = [];
  assert.equal(
    await judgeRuleViolation({ ...args, llm: fakeLlm({ ok: false, error: "boom" }) as any, warn: (m) => warnings.push(m) }),
    "ok",
    "LLM 障害は ok に倒す"
  );
  assert.equal(
    await judgeRuleViolation({ ...args, llm: fakeLlm({ ok: true, text: "???" }) as any, warn: (m) => warnings.push(m) }),
    "ok",
    "ラベル不明は ok に倒す"
  );
  assert.equal(warnings.length, 2, "degrade は warn を出す");
  console.log("  [ok] judgeRuleViolation: ラベル抽出 + degrade");
}

// ── gateRebut ─────────────────────────────────────────────────────────────────

{
  const llm = fakeLlm({ ok: true, text: "topic_shift" });
  const base = {
    issue,
    position,
    attack: "data" as const,
    groundId: "pro-G1",
    data: "d",
    warrant: "w",
    rebutText: "x",
    llm: llm as any,
  };
  const skipped = await gateRebut({ ...base, judge: false });
  assert.ok(skipped.accepted, "judge=false はアンカー検査だけで通す");
  assert.equal(llm.calls.length, 0, "judge=false は判定 LLM を呼ばない");

  const judged = await gateRebut({ ...base, judge: true });
  assert.deepEqual(judged, { accepted: false, foul: "topic_shift" });

  const unanchored = await gateRebut({ ...base, attack: null, judge: true });
  assert.deepEqual(unanchored, { accepted: false, foul: "unanchored" });
  assert.equal(llm.calls.length, 1, "アンカー違反なら判定 LLM を呼ばない");
  console.log("  [ok] gateRebut: アンカー検査 → 判定 LLM");
}

// ── 正規化 ────────────────────────────────────────────────────────────────────

{
  assert.equal(coerceInferenceKind("Induction"), "induction");
  assert.equal(coerceInferenceKind("アブダクション"), "abduction");
  assert.equal(coerceInferenceKind("analogy"), null);
  assert.equal(coerceAttackPoint("論拠"), "warrant");
  assert.equal(coerceAttackPoint("qualifier"), "qualifier");
  assert.equal(coerceAttackPoint(undefined), null);
  console.log("  [ok] premise-rules: 論拠の種類・突く要素の正規化");
}

console.log("rule-check tests: all passed");
