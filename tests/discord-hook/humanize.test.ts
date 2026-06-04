import assert from "node:assert/strict";

import { humanizeForDiscord } from "../../src/discord-hook/poster.js";

// 先頭の機械的ラベルを除去
assert.equal(humanizeForDiscord("反証: それは成り立たない。"), "それは成り立たない。");
assert.equal(humanizeForDiscord("反例：〇〇のケース。"), "〇〇のケース。");
assert.equal(humanizeForDiscord("弱点: ここが穴。"), "ここが穴。");

// 文末ごとに改行
assert.equal(humanizeForDiscord("これは良い。でも穴がある。"), "これは良い。\nでも穴がある。");
assert.equal(humanizeForDiscord("本当に?確かめた?"), "本当に?\n確かめた?");

// 既に改行済みなら二重にしない
assert.equal(humanizeForDiscord("A。\nB。"), "A。\nB。");

// ラベルが無い普通の文はそのまま (改行のみ)
assert.equal(humanizeForDiscord("それは違うんじゃない?"), "それは違うんじゃない?");

console.log("ok humanizeForDiscord");
console.log("discord-hook humanize test: passed");
