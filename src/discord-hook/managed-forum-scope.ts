// @spec Di管理フォーラムの受信境界
import { ChannelType } from "discord.js";

export type ManagedForumIds = ReadonlyMap<string, string>;
export interface ForumLocation { guildId?: string | null; channelId?: string | null; channel?: unknown }
export type FetchForumChannel = (channelId: string) => Promise<unknown>;
interface ChannelIdentity { id?: string; guildId?: string; parentId?: string | null; parent?: unknown; type?: number; isThread?: () => boolean }
function identity(value: unknown): ChannelIdentity | undefined {
  return value !== null && typeof value === "object" ? value as ChannelIdentity : undefined;
}
function matchesThread(channel: ChannelIdentity | undefined, guildId: string, forumId: string): boolean {
  return !!channel?.id && channel.guildId === guildId && channel.parentId === forumId && typeof channel.isThread === "function" && channel.isThread() === true;
}
/** Identity, not the forum's name or type alone, grants access. Empty scope denies all. */
export function isManagedForumThread(channel: unknown, forums: ManagedForumIds): boolean {
  const thread = identity(channel);
  const guildId = thread?.guildId;
  const forumId = guildId ? forums.get(guildId) : undefined;
  if (!guildId || !forumId || !matchesThread(thread, guildId, forumId)) return false;
  const parent = identity(thread?.parent);
  return parent?.id === forumId && parent.guildId === guildId && parent.type === ChannelType.GuildForum;
}
/** Fetch only unresolved identities in a configured guild, and fail closed on any error. */
export async function isManagedForumLocation(location: ForumLocation, forums: ManagedForumIds, fetchChannel: FetchForumChannel): Promise<boolean> {
  const guildId = location.guildId;
  const forumId = guildId ? forums.get(guildId) : undefined;
  if (!guildId || !forumId || !location.channelId) return false;
  try {
    const channel = identity(location.channel ?? await fetchChannel(location.channelId));
    if (channel?.id !== location.channelId || !matchesThread(channel, guildId, forumId)) return false;
    const parent = identity(channel.parent ?? await fetchChannel(forumId));
    return isManagedForumThread({ id: channel.id, guildId: channel.guildId, parentId: channel.parentId, parent, isThread: () => true }, forums);
  } catch {
    // Failed resolution cannot authorize ingestion, acknowledgement or state changes.
    return false;
  }
}
