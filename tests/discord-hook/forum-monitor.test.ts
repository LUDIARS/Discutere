import assert from "node:assert/strict";
import { ChannelType } from "discord.js";

import {
  formatForumSummary,
  isForumStarterMessage,
  isForumThreadChannel,
  parseDiscordScene,
} from "../../src/discord-hook/forum-monitor.js";

// ── parseDiscordScene ──
assert.deepEqual(parseDiscordScene("discord:111/222"), { guildId: "111", channelId: "222" });
assert.equal(parseDiscordScene("gap:abc"), null);
assert.equal(parseDiscordScene("discord:111"), null); // "/" 無し
assert.equal(parseDiscordScene("discord:/222"), null); // guild 空
assert.equal(parseDiscordScene("discord:111/"), null); // channel 空
assert.equal(parseDiscordScene(null), null);
assert.equal(parseDiscordScene(undefined), null);
console.log("ok parseDiscordScene");

// ── isForumThreadChannel ──
const forumThread = { isThread: () => true, parent: { type: ChannelType.GuildForum } };
const textThread = { isThread: () => true, parent: { type: ChannelType.GuildText } };
const plainChannel = { isThread: () => false, parent: null };
assert.equal(isForumThreadChannel(forumThread), true);
assert.equal(isForumThreadChannel(textThread), false); // 親がテキスト = フォーラムでない
assert.equal(isForumThreadChannel(plainChannel), false);
assert.equal(isForumThreadChannel(null), false);
assert.equal(isForumThreadChannel({ isThread: () => true, parent: null }), false);
console.log("ok isForumThreadChannel");

// ── isForumStarterMessage: フォーラムポストの starter は msg.id === channelId(thread.id) ──
assert.equal(isForumStarterMessage({ id: "999", channelId: "999" }), true);
assert.equal(isForumStarterMessage({ id: "1000", channelId: "999" }), false);
console.log("ok isForumStarterMessage");

// ── formatForumSummary ──
const s = formatForumSummary("ヴァンサバの面白さ", "結論本文です");
assert.ok(s.includes("収束"));
assert.ok(s.includes("ヴァンサバの面白さ"));
assert.ok(s.includes("結論本文です"));
assert.ok(formatForumSummary("題", "  ").includes("(まとめ未生成)")); // 空 summary
console.log("ok formatForumSummary");

console.log("forum-monitor tests: all passed");
