import assert from "node:assert/strict";

import {
  PAPER_GAP_FIELD_IDS,
  PAPER_GAP_MODAL_PREFIX,
  buildPaperGapModal,
  formatPaperGapReviewInstruction,
  isPaperGapType,
  parsePaperGapModalCustomId,
} from "../../src/discord-hook/paper-gap-ui.js";

assert.equal(isPaperGapType("user_reaction"), true);
assert.equal(isPaperGapType("unknown"), false);
console.log("ok paper gap type guard");

{
  const parsed = parsePaperGapModalCustomId(`${PAPER_GAP_MODAL_PREFIX}:user_reaction:ch123`);
  assert.deepEqual(parsed, { type: "user_reaction", channelId: "ch123" });
  assert.equal(parsePaperGapModalCustomId(`${PAPER_GAP_MODAL_PREFIX}:unknown:ch123`), null);
  assert.equal(parsePaperGapModalCustomId(`${PAPER_GAP_MODAL_PREFIX}:user_reaction:`), null);
  assert.equal(parsePaperGapModalCustomId("flow-modal:ch123"), null);
}
console.log("ok paper gap modal custom id parser");

{
  const modal = buildPaperGapModal({ type: "mechanics", channelId: "ch999" }).toJSON() as {
    custom_id: string;
    title: string;
    components: Array<{ components: Array<{ custom_id: string; required?: boolean }> }>;
  };
  assert.equal(modal.custom_id, `${PAPER_GAP_MODAL_PREFIX}:mechanics:ch999`);
  assert.ok(modal.title.includes("システム/メカニクス"));
  assert.deepEqual(
    modal.components.map((row) => row.components[0].custom_id),
    [PAPER_GAP_FIELD_IDS.target, PAPER_GAP_FIELD_IDS.detail, PAPER_GAP_FIELD_IDS.source]
  );
  assert.equal(modal.components[1].components[0].required, true);
}
console.log("ok paper gap modal builder");

{
  const text = formatPaperGapReviewInstruction({
    type: "user_reaction",
    target: "中盤のテンポ",
    detail: "プレイヤーは報酬間隔が長いと感じそうなので、テンポ懸念を追加する。",
    source: "想定ユーザ反応",
  });
  assert.ok(text.includes("ユーザの反応"));
  assert.ok(text.includes("対象: 中盤のテンポ"));
  assert.ok(text.includes("内容: プレイヤーは報酬間隔"));
  assert.ok(text.includes("根拠・情報ソース: 想定ユーザ反応"));
}
console.log("ok paper gap review instruction");

console.log("paper-gap-ui.test.ts: all passed");
