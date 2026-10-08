// @spec Di管理フォーラムの受信境界
import { Events, type AnyThreadChannel, type Client, type Interaction, type Message, type MessageReaction, type PartialMessageReaction, type PartialUser, type User } from "discord.js";
import { isManagedForumLocation, type FetchForumChannel, type ManagedForumIds } from "./managed-forum-scope.js";
import { parseFlowCustomBtnCustomId, parseFlowModalCustomId, parseFlowPickCustomId, parseFlowSettingsCustomId } from "./forum-flow-tags.js";
import { parsePaperGapModalCustomId } from "./paper-gap-ui.js";

export interface ForumEventHandlers {
  messageCreate(message: Message): void | Promise<void>;
  reactionAdd(reaction: MessageReaction | PartialMessageReaction, user: User | PartialUser): void | Promise<void>;
  interactionCreate(interaction: Interaction): void | Promise<void>;
  threadCreate?: (thread: AnyThreadChannel, newlyCreated: boolean) => void | Promise<void>;
}
/** A component cannot operate another thread's pending state, even inside the same forum. */
export function componentMatchesChannel(customId: string, channelId: string | null): boolean {
  const target = parseFlowCustomBtnCustomId(customId) ?? parseFlowModalCustomId(customId)
    ?? parseFlowPickCustomId(customId) ?? parseFlowSettingsCustomId(customId) ?? parsePaperGapModalCustomId(customId)?.channelId;
  return target == null || target === channelId;
}
/** The production gateway and fixtures bind the same guarded event listeners. */
export function bindManagedForumEvents(client: Pick<Client, "on">, forums: ManagedForumIds, fetchChannel: FetchForumChannel, handlers: ForumEventHandlers): void {
  client.on(Events.MessageCreate, async (message) => {
    if (message.author?.bot || message.reference) return;
    if (!await isManagedForumLocation({ guildId: message.guildId, channelId: message.channelId, channel: message.channel }, forums, fetchChannel)) return;
    try { await handlers.messageCreate(message); } catch { console.warn("  discord-gateway: managed message handler failed"); }
  });
  client.on(Events.MessageReactionAdd, async (reaction, user) => {
    if (user.bot) return;
    const message = reaction.message;
    if (!await isManagedForumLocation({ guildId: message.guildId, channelId: message.channelId, channel: message.channel }, forums, fetchChannel)) return;
    try { await handlers.reactionAdd(reaction, user); } catch { console.warn("  discord-gateway: managed reaction handler failed"); }
  });
  client.on(Events.InteractionCreate, async (interaction) => {
    if ("customId" in interaction && !componentMatchesChannel(interaction.customId, interaction.channelId)) return;
    if (!await isManagedForumLocation({ guildId: interaction.guildId, channelId: interaction.channelId, channel: interaction.channel }, forums, fetchChannel)) return;
    try { await handlers.interactionCreate(interaction); } catch { console.warn("  discord-gateway: managed interaction handler failed"); }
  });
  client.on(Events.ThreadCreate, async (thread, newlyCreated) => {
    if (!newlyCreated || !handlers.threadCreate) return;
    if (!await isManagedForumLocation({ guildId: thread.guildId, channelId: thread.id, channel: thread }, forums, fetchChannel)) return;
    try { await handlers.threadCreate(thread, newlyCreated); } catch { console.warn("  discord-gateway: managed thread handler failed"); }
  });
}
