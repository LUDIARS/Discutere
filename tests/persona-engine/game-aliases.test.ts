/**
 * ゲーム名の別名展開 (#301) のテスト。
 * タイトル "EN (JP)" の機械分解 + 静的略称辞書をマージし、略称⇄正式名・和名⇄英名で
 * 検索語を相互拡張できることを検証する。
 */
import assert from "node:assert/strict";

import {
  parseTitleAliases,
  buildAliasGroups,
  expandAliases,
} from "../../src/discatier-engine-adapter/game-aliases.js";

// ── parseTitleAliases ────────────────────────────────────────────────────────
assert.deepEqual(parseTitleAliases("Genshin Impact (原神)"), ["Genshin Impact", "原神"]);
assert.deepEqual(parseTitleAliases("Shadow of the Colossus (ワンダと巨像)"), [
  "Shadow of the Colossus",
  "ワンダと巨像",
]);
assert.deepEqual(parseTitleAliases("モンスターストライク"), ["モンスターストライク"]);
console.log("ok parseTitleAliases");

// ── buildAliasGroups: タイトル + 静的略称をマージ ────────────────────────────
{
  const groups = buildAliasGroups(["Genshin Impact (原神)", "モンスターストライク", "Hollow Knight"]);
  const lower = groups.map((g) => g.map((x) => x.toLowerCase()));

  // 原神グループ: タイトル(原神/Genshin Impact) と 静的(genshin) が原神で繋がる。
  const genshin = lower.find((g) => g.includes("原神"));
  assert.ok(genshin, "原神グループがある");
  assert.ok(genshin!.includes("genshin"), "静的別名 genshin がマージされる");

  // モンストグループ: タイトル(単独) と 静的(モンスト) が繋がる。
  const monst = lower.find((g) => g.includes("モンスト"));
  assert.ok(monst && monst.includes("モンスターストライク"), "モンスト⇄モンスターストライクが同一グループ");

  // ホロウナイトグループ: タイトル(英名のみ) と 静的(和名/略称) が繋がる。
  const hollow = lower.find((g) => g.includes("ホロナイ"));
  assert.ok(hollow && hollow.includes("hollow knight"), "ホロナイ⇄Hollow Knight が同一グループ");

  // 別名を持たない単独語はグループにしない。
  assert.ok(!lower.some((g) => g.length < 2), "1 要素グループは落とす");
}
console.log("ok buildAliasGroups");

// ── expandAliases: 双方向に略称⇄正式名を補う ────────────────────────────────
{
  const groups = buildAliasGroups(["Genshin Impact (原神)", "モンスターストライク"]);

  const fromFull = expandAliases(["モンスターストライク"], groups);
  assert.ok(fromFull.includes("モンスト"), "正式名 → 略称を補う");

  const fromAbbr = expandAliases(["モンスト"], groups);
  assert.ok(fromAbbr.includes("モンスターストライク"), "略称 → 正式名を補う");

  const fromJa = expandAliases(["原神"], groups);
  assert.ok(fromJa.includes("genshin"), "和名 → 英名/別名を補う");

  // 無関係な語は素通し (誤った別名を足さない)。
  assert.deepEqual(expandAliases(["無関係なテーマ"], groups), ["無関係なテーマ"]);

  // 元の語は必ず保持する。
  assert.ok(expandAliases(["モンスト", "周回"], groups).includes("周回"));
}
console.log("ok expandAliases");

console.log("game-aliases tests: all passed");

// ── extraGroups (Ludus 用語辞書) をマージできる ─────────────────────────────
{
  const groups = buildAliasGroups(
    ["モンスターストライク"],
    [["経験値", "XP", "EXP"], ["単独語のみ"]]
  );
  const xp = expandAliases(["経験値"], groups);
  assert.ok(xp.includes("xp") && xp.includes("exp"), "用語辞書の言い換えを補う");
  // 1 要素の extra グループは展開に寄与しない (buildAliasGroups が落とす)。
  assert.deepEqual(expandAliases(["単独語のみ"], groups), ["単独語のみ"]);
  // 既存のゲーム名グループと独立に共存する。
  assert.ok(expandAliases(["モンスト"], groups).includes("モンスターストライク"));
}
console.log("ok buildAliasGroups extraGroups");
