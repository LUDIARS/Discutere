/**
 * extractGameFeedback のダミー注入テスト (discord.js Message を最小 duck-type で模す)。
 */
import assert from "node:assert/strict";
import { ChannelType, type Message } from "discord.js";

import { extractGameFeedback } from "../../src/discord-hook/game-feedback-channel.js";

const CATEGORY = "ゲーム感想";

function dummyMsg(opts: {
  channelType: ChannelType;
  channelName: string;
  parentName: string | null;
  content: string;
}): Message {
  const parent =
    opts.parentName === null
      ? null
      : { type: ChannelType.GuildCategory, name: opts.parentName };
  return {
    content: opts.content,
    createdTimestamp: 1_700_000_000_000,
    channel: { type: opts.channelType, name: opts.channelName, parent },
  } as unknown as Message;
}

// 1. 感想カテゴリ配下のテキストチャンネル → 抽出される (gameTitle = チャンネル名)
const ok = extractGameFeedback(
  dummyMsg({ channelType: ChannelType.GuildText, channelName: "vampire-survivors", parentName: CATEGORY, content: "中毒性が高い" }),
  CATEGORY
);
assert.ok(ok, "感想カテゴリ配下は抽出される");
assert.equal(ok!.gameTitle, "vampire-survivors");
assert.equal(ok!.content, "中毒性が高い");
console.log("ok extract (感想カテゴリ配下)");

// 2. 別カテゴリ → null
assert.equal(
  extractGameFeedback(
    dummyMsg({ channelType: ChannelType.GuildText, channelName: "general", parentName: "雑談", content: "やあ" }),
    CATEGORY
  ),
  null,
  "別カテゴリは対象外"
);
console.log("ok 別カテゴリは収集しない");

// 3. 親カテゴリ無し → null
assert.equal(
  extractGameFeedback(
    dummyMsg({ channelType: ChannelType.GuildText, channelName: "x", parentName: null, content: "y" }),
    CATEGORY
  ),
  null
);
// 4. 空本文 → null
assert.equal(
  extractGameFeedback(
    dummyMsg({ channelType: ChannelType.GuildText, channelName: "g", parentName: CATEGORY, content: "   " }),
    CATEGORY
  ),
  null
);
console.log("ok 親カテゴリ無し / 空本文は収集しない");

console.log("feedback/extract.test.ts: all passed");
