// @spec Di管理フォーラムの受信境界
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { ChannelType, Events, type Client } from "discord.js";
import { bindManagedForumEvents, componentMatchesChannel } from "../../src/discord-hook/forum-event-gate.js";

type Listener = (...args: unknown[]) => Promise<void>;
const listeners = new Map<string, Listener>();
const client = { on: (event: string, listener: Listener) => { listeners.set(event, listener); return client; } } as unknown as Pick<Client, "on">;
const forums = new Map<string, string>();
let callbacks = 0;
let discordWrites = 0;
let fetches = 0;
const dispatched = (): void => { callbacks++; discordWrites++; };
bindManagedForumEvents(client, forums, async () => { fetches++; throw new Error("unexpected lookup"); }, {
  messageCreate: dispatched, reactionAdd: dispatched, interactionCreate: dispatched, threadCreate: dispatched,
});
function channelFixture(parentId = "di-a", guildId = "guild-a") {
  return { id: "post", guildId, parentId, isThread: () => true, parent: { id: parentId, guildId, type: ChannelType.GuildForum } };
}
function messageFixture(channel = channelFixture()) {
  return { id: "message", channelId: channel.id, guildId: channel.guildId, channel, reference: null, author: { bot: false }, content: "@Di https://example.test", mentions: {}, react: dispatched, reply: dispatched };
}
async function emitFixture(channel = channelFixture()): Promise<void> {
  const message = messageFixture(channel);
  await listeners.get(Events.MessageCreate)!(message);
  await listeners.get(Events.ThreadCreate)!(channel, true);
  await listeners.get(Events.MessageReactionAdd)!({ message, emoji: { name: "✅" } }, { bot: false });
  for (const kind of ["button", "modal", "select", "slash"]) {
    await listeners.get(Events.InteractionCreate)!({ guildId: channel.guildId, channelId: channel.id, channel, ...(kind === "slash" ? { commandName: "mechanics-check" } : { customId: "unrelated" }), reply: dispatched, update: dispatched, showModal: dispatched });
  }
}
// Ready has not resolved the managed IDs yet: no ingress callback or Discord lookup.
await emitFixture();
assert.deepEqual([callbacks, discordWrites, fetches], [0, 0, 0]);
forums.set("guild-a", "di-a"); forums.set("guild-b", "di-b");
for (const channel of [channelFixture("other"), channelFixture("di-a", "unconfigured"), channelFixture("di-a", "guild-b"), { ...channelFixture(), isThread: () => false }, { ...channelFixture(), parent: { id: "di-a", guildId: "guild-a", type: ChannelType.GuildText } }]) await emitFixture(channel);
await listeners.get(Events.MessageCreate)!({ ...messageFixture(), guildId: null, channelId: "dm", channel: null });
await listeners.get(Events.InteractionCreate)!({ guildId: null, channelId: "dm", channel: null, commandName: "discutere-backup" });
assert.deepEqual([callbacks, discordWrites, fetches], [0, 0, 0]);
await listeners.get(Events.MessageCreate)!({ ...messageFixture(), reference: { messageId: "replied-to" } });
await listeners.get(Events.MessageCreate)!({ ...messageFixture(), author: { bot: true } });
await listeners.get(Events.ThreadCreate)!(channelFixture(), false);
await listeners.get(Events.MessageReactionAdd)!({ message: messageFixture() }, { bot: true });
assert.deepEqual([callbacks, discordWrites, fetches], [0, 0, 0]);
for (const prefix of ["flow-custom", "flow-modal", "flow-pick", "flow-settings", "paper-gap:mechanics"]) {
  const customId = `${prefix}:another-post`;
  assert.equal(componentMatchesChannel(customId, "post"), false);
  await listeners.get(Events.InteractionCreate)!({ guildId: "guild-a", channelId: "post", channel: channelFixture(), customId, reply: dispatched, update: dispatched, showModal: dispatched });
}
assert.deepEqual([callbacks, discordWrites, fetches], [0, 0, 0]);
assert.equal(componentMatchesChannel("flow-pick:post", "post"), true);
await emitFixture();
assert.deepEqual([callbacks, discordWrites, fetches], [7, 7, 0]);
await emitFixture(channelFixture("di-b", "guild-b"));
assert.deepEqual([callbacks, discordWrites, fetches], [14, 14, 0]);
// Tie the behavioral fixture to the binding actually used by the production gateway.
const gateway = readFileSync(new URL("../../src/discord-hook/gateway.ts", import.meta.url), "utf8");
assert.match(gateway, /bindManagedForumEvents\(client, managedForumIds, fetchForumChannel,/);
assert.doesNotMatch(gateway, /client\.on\(Events\.(?:MessageCreate|MessageReactionAdd|InteractionCreate|ThreadCreate)/);
