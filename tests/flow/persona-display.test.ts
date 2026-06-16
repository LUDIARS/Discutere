/**
 * 表示名 composer テスト (item2/4): 「名前 (ロール名/憑依ペルソナ)」。
 * 憑依が無ければ括弧内はロールのみ。スタンス優先、無ければ role。
 */

import assert from "node:assert/strict";

const { composeDisplayName, stanceLabel, roleLabel } = await import("../../src/flow/persona-display.js");

// ── スタンス → ロール名 ──────────────────────────────────────────────────────
{
  assert.equal(stanceLabel("pro"), "賛成派");
  assert.equal(stanceLabel("con"), "反対派");
  assert.equal(stanceLabel("opinion"), "意見屋");
  assert.equal(stanceLabel("neutral"), "進行役");
  assert.equal(roleLabel("facilitator"), "進行役");
  assert.equal(roleLabel("debater"), "論者");
  assert.equal(roleLabel("opinion"), "意見屋");
  console.log("  [ok] stanceLabel / roleLabel");
}

// ── 憑依なし: 名前 (ロール) ───────────────────────────────────────────────────
{
  assert.equal(composeDisplayName({ name: "陽介", stance: "opinion" }), "陽介 (意見屋)");
  assert.equal(composeDisplayName({ name: "凛", stance: "pro" }), "凛 (賛成派)");
  assert.equal(composeDisplayName({ name: "舞", stance: "con" }), "舞 (反対派)");
  assert.equal(composeDisplayName({ name: "ハル", stance: "neutral" }), "ハル (進行役)");
  // スタンス無し → role フォールバック
  assert.equal(composeDisplayName({ name: "葵", role: "debater" }), "葵 (論者)");
  // 空文字 possession は憑依なし扱い
  assert.equal(composeDisplayName({ name: "蓮", stance: "opinion", possessionName: "  " }), "蓮 (意見屋)");
  console.log("  [ok] 憑依なし: 名前 (ロール)");
}

// ── 憑依あり: 名前 (ロール/憑依) ──────────────────────────────────────────────
{
  assert.equal(
    composeDisplayName({ name: "陽介", stance: "opinion", possessionName: "論者#a1b2c3" }),
    "陽介 (意見屋/論者#a1b2c3)"
  );
  assert.equal(
    composeDisplayName({ name: "結衣", stance: "pro", possessionName: "論者#ffff" }),
    "結衣 (賛成派/論者#ffff)"
  );
  console.log("  [ok] 憑依あり: 名前 (ロール/憑依)");
}

// ── ロールもスタンスも無い → 素の名前 ────────────────────────────────────────
{
  assert.equal(composeDisplayName({ name: "翔" }), "翔");
  console.log("  [ok] ロール無し → 名前のみ");
}

console.log("persona-display tests: all passed");
