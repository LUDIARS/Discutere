/**
 * システムカテゴリ配下の通知チャンネルを ensure する (id=61)。
 *
 * 「データ追加」 など運用通知用のチャンネルを、 指定 guild の「システム」 カテゴリ配下に
 * 用意する。 カテゴリ / チャンネルが無ければ作成する (Manage Channels 権限が必要)。
 * 解決済みチャンネルは guild ごとにキャッシュして毎回の検索/作成を避ける。
 */

import {
  ChannelType,
  PermissionFlagsBits,
  type Client,
  type Guild,
  type TextChannel,
} from "discord.js";

export interface EnsureSystemChannelOptions {
  guildId: string;
  /** 親カテゴリ名 (既定「システム」) */
  categoryName?: string;
  /** チャンネル名 (既定「データ追加」) */
  channelName?: string;
}

const cache = new Map<string, TextChannel>();

function normalize(name: string): string {
  // Discord はチャンネル名を小文字化・空白をハイフン化するので照合を緩める。
  return name.toLowerCase().replace(/\s+/g, "-");
}

async function findOrCreateCategory(guild: Guild, name: string) {
  const existing = guild.channels.cache.find(
    (c) => c.type === ChannelType.GuildCategory && c.name === name
  );
  if (existing) return existing;
  return guild.channels.create({ name, type: ChannelType.GuildCategory });
}

/**
 * 「<category>/<channel>」 を ensure して TextChannel を返す。
 * 権限不足 / 取得失敗時は null (呼び出し側は通知を skip)。
 */
export async function ensureSystemChannel(
  client: Client,
  opts: EnsureSystemChannelOptions
): Promise<TextChannel | null> {
  const categoryName = opts.categoryName ?? "システム";
  const channelName = opts.channelName ?? "データ追加";
  const key = `${opts.guildId}:${categoryName}/${channelName}`;
  const cached = cache.get(key);
  if (cached) return cached;

  try {
    const guild = client.guilds.cache.get(opts.guildId) ?? (await client.guilds.fetch(opts.guildId));
    if (!guild) return null;

    // Bot に Manage Channels が無ければ作成できない → 既存チャンネルだけ探す。
    const me = guild.members.me ?? (await guild.members.fetchMe());
    const canManage = me?.permissions.has(PermissionFlagsBits.ManageChannels) ?? false;

    const target = normalize(channelName);
    const existing = guild.channels.cache.find(
      (c) => c.type === ChannelType.GuildText && normalize(c.name) === target
    ) as TextChannel | undefined;
    if (existing) {
      cache.set(key, existing);
      return existing;
    }
    if (!canManage) return null;

    const category = await findOrCreateCategory(guild, categoryName);
    const created = (await guild.channels.create({
      name: channelName,
      type: ChannelType.GuildText,
      parent: category.id,
      topic: "外部議論データの取り込み通知 (Discutere クロール)",
    })) as TextChannel;
    cache.set(key, created);
    return created;
  } catch {
    return null;
  }
}

/** テスト/再接続用にキャッシュをクリア。 */
export function clearSystemChannelCache(): void {
  cache.clear();
}
