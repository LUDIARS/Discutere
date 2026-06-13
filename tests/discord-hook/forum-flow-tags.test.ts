import assert from "node:assert/strict";

import {
  ALL_FLOW_TAG_NAMES,
  FLOW_ASPECT_TAG_NAMES,
  FLOW_KIND_TAG_NAMES,
  FLOW_PICK_PREFIX,
  buildFlowPickMenu,
  mergeForumTags,
  parseFlowPickCustomId,
} from "../../src/discord-hook/forum-flow-tags.js";
import { parseFlowKind } from "../../src/flow/dispatch.js";
import { canCollectExternal } from "../../src/flow/tags.js";

// ── 定義済みタグ集合 ──
assert.deepEqual([...FLOW_KIND_TAG_NAMES], ["議論", "改善", "学習", "壁打ち"]);
assert.deepEqual([...FLOW_ASPECT_TAG_NAMES], ["機密", "内部", "運用", "開発"]);
assert.equal(ALL_FLOW_TAG_NAMES.length, 8);
console.log("ok flow tag sets");

// ── 議論タイプタグは parseFlowKind で必ず解決できる (フロー起動の前提) ──
for (const name of FLOW_KIND_TAG_NAMES) {
  assert.ok(parseFlowKind(name), `議論タイプタグ "${name}" が FlowKind に解決できない`);
}
// 観点タグは FlowKind ではない (誤って flow 扱いしない)。
for (const name of FLOW_ASPECT_TAG_NAMES) {
  assert.equal(parseFlowKind(name), null, `観点タグ "${name}" が誤って FlowKind 扱い`);
}
// 観点タグの収集可否 (機密/内部 は収集禁止)。
assert.equal(canCollectExternal(["開発"]), true);
assert.equal(canCollectExternal(["機密"]), false);
assert.equal(canCollectExternal(["内部", "運用"]), false);
console.log("ok flow tag semantics");

// ── mergeForumTags ──
{
  const fresh = mergeForumTags([]);
  assert.deepEqual(
    fresh.tags.map((t) => t.name),
    [...ALL_FLOW_TAG_NAMES]
  );
  assert.deepEqual(fresh.added, [...ALL_FLOW_TAG_NAMES]);

  // 既存タグは温存し、不足分のみ末尾に足す。
  const partial = mergeForumTags([{ name: "議論" }, { name: "雑談" }]);
  assert.deepEqual(partial.tags.map((t) => t.name).slice(0, 2), ["議論", "雑談"]);
  assert.ok(!partial.added.includes("議論"));
  assert.ok(partial.added.includes("壁打ち"));
  assert.ok(partial.tags.some((t) => t.name === "雑談")); // 無関係タグも残る

  // 既に全部あれば追加なし。
  const full = mergeForumTags(ALL_FLOW_TAG_NAMES.map((name) => ({ name })));
  assert.deepEqual(full.added, []);

  // 20 件上限を超えない。
  const many = Array.from({ length: 19 }, (_, i) => ({ name: `t${i}` }));
  const capped = mergeForumTags(many);
  assert.equal(capped.tags.length, 20);
  assert.equal(capped.added.length, 1);
}
console.log("ok mergeForumTags");

// ── parseFlowPickCustomId ──
assert.equal(parseFlowPickCustomId(`${FLOW_PICK_PREFIX}:12345`), "12345");
assert.equal(parseFlowPickCustomId("debate:cont:xx"), null);
assert.equal(parseFlowPickCustomId(`${FLOW_PICK_PREFIX}:`), null); // threadId 空
console.log("ok parseFlowPickCustomId");

// ── buildFlowPickMenu ──
{
  const row = buildFlowPickMenu("999").toJSON() as {
    components: Array<{ type: number; custom_id: string; options: Array<{ value: string }> }>;
  };
  const select = row.components[0];
  assert.equal(select.custom_id, `${FLOW_PICK_PREFIX}:999`);
  assert.deepEqual(
    select.options.map((o) => o.value),
    [...FLOW_KIND_TAG_NAMES]
  );
}
console.log("ok buildFlowPickMenu");

console.log("forum-flow-tags tests: all passed");
