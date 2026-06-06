import assert from "node:assert/strict";
import { ChannelType } from "discord.js";

import {
  formatForumSummary,
  isForumStarterMessage,
  isForumThreadChannel,
  parseDiscordScene,
  pickForumDirection,
} from "../../src/discord-hook/forum-monitor.js";
import { forumDirectionDirective } from "../../src/discord-hook/auto-discussion.js";

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

// ── pickForumDirection: タグで議論の方向性を決める ──
assert.equal(pickForumDirection([]), "fun"); // タグ無し → 既定=面白さ
assert.equal(pickForumDirection(["面白さ"]), "fun");
assert.equal(pickForumDirection(["改善提案"]), "improvement");
assert.equal(pickForumDirection(["改善"]), "improvement"); // 部分一致
assert.equal(pickForumDirection(["雑談"]), "fun"); // 未一致タグ → 既定=面白さ
// 改善が優先 (両方付いていても改善方向)
assert.equal(pickForumDirection(["面白さ", "改善提案"]), "improvement");
// config 上書き
assert.equal(
  pickForumDirection(["proposal"], { improvementTagNames: ["proposal"], funTagNames: ["fun"] }),
  "improvement"
);
console.log("ok pickForumDirection");

// ── forumDirectionDirective: 方向ごとに別ディレクティブ ──
assert.ok(forumDirectionDirective("improvement").includes("改善提案"));
assert.ok(forumDirectionDirective("fun").includes("面白さ"));
assert.notEqual(forumDirectionDirective("improvement"), forumDirectionDirective("fun"));
console.log("ok forumDirectionDirective");

console.log("forum-monitor tests: all passed");
