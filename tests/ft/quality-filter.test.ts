import assert from "node:assert/strict";

import { createQualityFilter } from "../../scripts/ft/quality-filter.js";

const SYSTEM =
  "あなたは議論ペルソナ「論客A」です。\n" +
  "与えられた議題について、外部の声を根拠に自分の立場から具体的に論じてください。\n" +
  "short line";

const LONG_A =
  "引っ張りアクションの気持ち良さは、狙い通りに反射が決まった時の予測と結果の一致から来ていると考える。";
const LONG_B =
  "ガチャの排出率よりも、天井までの距離が見えることの方が課金の納得感には効いているはずだ。";

// ── too_short: 短文の相槌を落とす ────────────────────────────────────────────
{
  const filter = createQualityFilter();
  assert.deepEqual(filter({ reqId: "1", workerId: "w1", system: SYSTEM, assistant: "なるほど、同意です。" }), {
    accepted: false,
    reason: "too_short",
  });
  assert.equal(filter({ reqId: "2", workerId: "w1", system: SYSTEM, assistant: LONG_A }).accepted, true);
}
console.log("ok too_short");

// ── repetition: 同一 worker の直前採用とほぼ同文なら落とす ───────────────────
{
  const filter = createQualityFilter();
  assert.equal(filter({ reqId: "1", workerId: "w1", system: SYSTEM, assistant: LONG_A }).accepted, true);
  assert.deepEqual(
    filter({ reqId: "2", workerId: "w1", system: SYSTEM, assistant: LONG_A + "!" }),
    { accepted: false, reason: "repetition" }
  );
  // 別 worker の同文は落とさない (比較は worker 単位)。
  assert.equal(filter({ reqId: "3", workerId: "w2", system: SYSTEM, assistant: LONG_A }).accepted, true);
  // 同 worker でも別内容なら通る。
  assert.equal(filter({ reqId: "4", workerId: "w1", system: SYSTEM, assistant: LONG_B }).accepted, true);
}
console.log("ok repetition");

// ── role_leak: system の長い行を復唱したら落とす ─────────────────────────────
{
  const filter = createQualityFilter();
  const leak = "前置きとして、与えられた議題について、外部の声を根拠に自分の立場から具体的に論じてください。と言われたので論じる。";
  assert.deepEqual(filter({ reqId: "1", workerId: "w1", system: SYSTEM, assistant: leak }), {
    accepted: false,
    reason: "role_leak",
  });
  // 短い行 ("short line") の一致はリーク扱いしない。
  assert.equal(
    filter({ reqId: "2", workerId: "w1", system: SYSTEM, assistant: `${LONG_A} short line` }).accepted,
    true
  );
}
console.log("ok role_leak");

// ── group_cap: グループ採用上限 (sessionId 無しは workerId 単位) ──────────────
{
  const filter = createQualityFilter({ groupCap: 2 });
  assert.equal(filter({ reqId: "1", workerId: "w1", system: SYSTEM, assistant: LONG_A }).accepted, true);
  assert.equal(filter({ reqId: "2", workerId: "w1", system: SYSTEM, assistant: LONG_B }).accepted, true);
  assert.deepEqual(
    filter({ reqId: "3", workerId: "w1", system: SYSTEM, assistant: `${LONG_A} さらに別の論点も足す。` }),
    { accepted: false, reason: "group_cap" }
  );
  // sessionId があればそちらの単位で数える。
  const bySession = createQualityFilter({ groupCap: 1 });
  assert.equal(
    bySession({ reqId: "1", workerId: "w1", system: SYSTEM, assistant: LONG_A, sessionId: "s1" }).accepted,
    true
  );
  assert.deepEqual(
    bySession({ reqId: "2", workerId: "w2", system: SYSTEM, assistant: LONG_B, sessionId: "s1" }),
    { accepted: false, reason: "group_cap" }
  );
}
console.log("ok group_cap");

// ── 既定でグループ上限は無効 (groupCap=0) ────────────────────────────────────
{
  const filter = createQualityFilter();
  const texts = [LONG_A, LONG_B, `${LONG_A} 追加の視点。`, `${LONG_B} 追加の視点。`];
  for (let i = 0; i < texts.length; i += 1) {
    assert.equal(
      filter({ reqId: String(i), workerId: "w1", system: SYSTEM, assistant: texts[i] }).accepted,
      true,
      `groupCap=0 では上限で落ちない (${i})`
    );
  }
}
console.log("ok groupCap disabled by default");

console.log("ft quality-filter tests: all passed");
