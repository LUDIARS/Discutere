// @spec Di管理フォーラムの受信境界
import assert from "node:assert/strict";
import { ChannelType } from "discord.js";
import { isManagedForumLocation, isManagedForumThread } from "../../src/discord-hook/managed-forum-scope.js";

const forums = new Map([["guild-a", "di-a"], ["guild-b", "di-b"]]);
function threadFixture(guildId = "guild-a", parentId = "di-a") {
  return { id: "post", guildId, parentId, isThread: () => true, parent: { id: parentId, guildId, type: ChannelType.GuildForum } };
}
let fetches = 0;
const noFetch = async (): Promise<unknown> => { fetches++; throw new Error("unexpected fetch"); };
assert.equal(isManagedForumThread(threadFixture(), forums), true);
assert.equal(isManagedForumThread(threadFixture("guild-b", "di-b"), forums), true);
for (const channel of [threadFixture("guild-a", "other"), threadFixture("other-guild", "di-a"), threadFixture("guild-b", "di-a"), { ...threadFixture(), isThread: () => false }, { ...threadFixture(), parent: { id: "di-a", guildId: "guild-a", type: ChannelType.GuildText } }]) {
  assert.equal(isManagedForumThread(channel, forums), false);
  assert.equal(await isManagedForumLocation({ guildId: channel.guildId, channelId: channel.id, channel }, forums, noFetch), false);
}
assert.equal(fetches, 0);
assert.equal(await isManagedForumLocation({ guildId: null, channelId: "dm" }, forums, noFetch), false);
assert.equal(await isManagedForumLocation({ guildId: "guild-a", channelId: "post" }, new Map(), noFetch), false);
assert.equal(fetches, 0);
const uncachedParent = { ...threadFixture(), parent: null };
assert.equal(await isManagedForumLocation({ guildId: "guild-a", channelId: "post", channel: uncachedParent }, forums, async (id) => {
  assert.equal(id, "di-a"); fetches++; return threadFixture().parent;
}), true);
assert.equal(fetches, 1);
for (const parent of [null, { id: "other", guildId: "guild-a", type: ChannelType.GuildForum }, { id: "di-a", guildId: "other-guild", type: ChannelType.GuildForum }, { id: "di-a", guildId: "guild-a", type: ChannelType.GuildText }]) {
  assert.equal(await isManagedForumLocation({ guildId: "guild-a", channelId: "post", channel: uncachedParent }, forums, async () => parent), false);
}
assert.equal(await isManagedForumLocation({ guildId: "guild-a", channelId: "post", channel: uncachedParent }, forums, async () => { throw new Error("cannot fetch parent"); }), false);
assert.equal(await isManagedForumLocation({ guildId: "guild-a", channelId: "post" }, forums, async (id) => id === "post" ? threadFixture() : null), true);
assert.equal(await isManagedForumLocation({ guildId: "guild-a", channelId: "wrong", channel: threadFixture() }, forums, noFetch), false);
