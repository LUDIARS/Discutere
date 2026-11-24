import assert from "node:assert/strict";
import { buildChatItems, seededShuffle } from "../../scripts/creator-insight/life-script-items.js";
import { questionnaire } from "./fixtures.js";

const q = questionnaire();
const chat = buildChatItems(q);

// 51 問を 1 回ずつ、番号つきで出す。
assert.equal(chat.total, 51);
assert.equal(chat.items.length, 51);
assert.deepEqual(chat.items.map((i) => i.n), Array.from({ length: 51 }, (_, i) => i + 1));
assert.equal(new Set(chat.items.map((i) => i.id)).size, 51);

// 同じ設問 JSON からは毎回同じ順番。
assert.deepEqual(buildChatItems(q).items.map((i) => i.id), chat.items.map((i) => i.id));

// セクションの中だけで並べ替え、セクションの順番は保つ (ドライバー 20 → プロセス 18 → 構え 8 → 場面 5)。
const prefixes = chat.items.map((i) => i.id.split("-")[0]);
assert.deepEqual([...new Set(prefixes)], ["D", "S", "P", "FC"]);
// 同じ尺度の設問が設問 JSON の並び (尺度ごとにまとまった順) のままにはならない。
const drivers = chat.items.filter((i) => i.id.startsWith("D-")).map((i) => i.id);
assert.notDeepEqual(drivers, q.sections[0]!.items.map((i) => i.id));

// 答える人に尺度名・逆転の有無を見せない。
const json = JSON.stringify(chat);
assert.ok(!json.includes("be_perfect") && !json.includes("reverse"));

// likert は 4 段階、場面は 5 択。
for (const item of chat.items) {
  assert.equal(item.choices.length, item.kind === "likert" ? 4 : 5);
}
assert.ok(chat.prompt.length > 0);

// 並べ替えは種だけで決まり、要素を落とさない。
assert.deepEqual(seededShuffle([1, 2, 3, 4, 5], "s"), seededShuffle([1, 2, 3, 4, 5], "s"));
assert.deepEqual([...seededShuffle([1, 2, 3, 4, 5], "s")].sort(), [1, 2, 3, 4, 5]);
