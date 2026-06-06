/**
 * フォーラム議論の入口 + 収束クローズ (フォーラム集約)。
 *
 * Di の議論を Discord **フォーラムチャンネル** に集約する。フォーラムを「議論カテゴリ」
 * として使い、guild 内の全フォーラムを監視する:
 *   - フォーラムの新規ポスト (ThreadCreate, 親=GuildForum) の **最初の投稿 (starter)** で
 *     議論をトリガーする (command-router.routeForumPost 経由 → auto-discussion)。
 *   - ポスト内の後続投稿 (MessageCreate, starter 以外) は進行中議論への参加者発言になる。
 *   - 議論が **収束** したら finalizeForumPost でポストを archive + lock し、まとめを
 *     「まとめ投稿」チャンネルへ転記する。
 *
 * discord.js 依存はここに閉じ込め、取り込みロジック自体は transport 非依存の
 * command-router (routeForumPost) に委譲する (SRP)。
 */

import {
  ChannelType,
  type AnyThreadChannel,
  type Client,
  type Message,
} from "discord.js";

import { normalizeDiscordInboundMessage } from "./normalize.js";
import { routeForumPost, type CommandRouterDeps } from "./command-router.js";
import { ensureSystemChannel } from "./system-channel.js";

/** scene 文字列 "discord:<guildId>/<channelId>" を分解。形式不正なら null。 */
export function parseDiscordScene(
  scene: string | null | undefined
): { guildId: string; channelId: string } | null {
  if (!scene || !scene.startsWith("discord:")) return null;
  const rest = scene.slice("discord:".length);
  const slash = rest.indexOf("/");
  if (slash <= 0) return null;
  const guildId = rest.slice(0, slash);
  const channelId = rest.slice(slash + 1);
  if (!guildId || !channelId) return null;
  return { guildId, channelId };
}

/**
 * メッセージがフォーラムスレッド内のものか (= 親が GuildForum のスレッド)。
 * discord.js channel を受け、判定できなければ false。
 */
export function isForumThreadChannel(channel: unknown): boolean {
  const c = channel as { isThread?: () => boolean; parent?: { type?: number } | null } | null;
  if (!c?.isThread?.()) return false;
  return c.parent?.type === ChannelType.GuildForum;
}

/**
 * フォーラムポストの最初の投稿 (starter) か。
 * Discord ではフォーラムポストの開始メッセージ id はスレッド (= channel) id と一致する。
 */
export function isForumStarterMessage(msg: { id: string; channelId: string }): boolean {
  return msg.id === msg.channelId;
}

/** まとめ投稿チャンネル向けの収束まとめ整形。 */
export function formatForumSummary(title: string, summary: string): string {
  const body = summary.trim() || "(まとめ未生成)";
  return `✅ **収束**: ${title}\n${body}`;
}

export interface ForumMonitorDeps {
  /** routeForumPost に渡す共通 deps (workspaceId / classifyInboundMessage など)。 */
  router: CommandRouterDeps;
}

/**
 * フォーラムポスト新規作成 (ThreadCreate, 親=GuildForum) を議論として起動する。
 * 最初の投稿 (starter) を取得して routeForumPost(isStarter=true) に流し、議論が立てば
 * starter に 👀 を付ける。fetchStarterMessage は作成直後に取りこぼすことがあるため 1 回リトライ。
 */
export async function handleForumThreadCreate(
  thread: AnyThreadChannel,
  deps: ForumMonitorDeps
): Promise<void> {
  // 親が未キャッシュのことがあるので、不明なら親を fetch して種別を確かめる。
  let parentType: ChannelType | undefined = thread.parent?.type;
  if (parentType === undefined && thread.parentId) {
    try {
      const parent = await thread.client.channels.fetch(thread.parentId);
      parentType = parent?.type;
    } catch {
      /* 取得不可なら判定不能 → skip */
    }
  }
  if (parentType !== ChannelType.GuildForum) return;
  const guildId = thread.guildId ?? thread.guild?.id;
  if (!guildId) return;

  const starter = await fetchStarterWithRetry(thread);
  if (!starter) {
    console.warn(`  discord-forum: starter message 取得不可 (thread=${thread.id})`);
    return;
  }
  if (starter.author?.bot) return;

  const normalized = normalizeDiscordInboundMessage({
    id: starter.id,
    channel_id: thread.id, // session/scene を thread に紐付ける
    content: starter.content,
    timestamp: starter.createdAt.toISOString(),
    author: {
      id: starter.author.id,
      username: starter.author.username,
      bot: starter.author.bot,
    },
    mentions: starter.mentions.users.map((u) => ({ id: u.id, username: u.username })),
  });
  if (!normalized) return;

  try {
    const { ingested, seed } = routeForumPost(normalized, guildId, deps.router, { isStarter: true });
    if (ingested && seed) {
      const started = await seed;
      if (started) await starter.react("👀").catch(() => {});
    }
  } catch (err) {
    console.warn(`  discord-forum: thread-create route failed: ${(err as Error).message}`);
  }
}

/**
 * フォーラムスレッド内の後続投稿 (starter 以外) を進行中議論へ取り込む。
 * 新規 gap は立てず utterance のみ記録 → event-bridge が人間発言としてミラーする。
 */
export function handleForumReply(msg: Message, deps: ForumMonitorDeps): void {
  const guildId = msg.guildId;
  if (!guildId) return;
  const normalized = normalizeDiscordInboundMessage({
    id: msg.id,
    channel_id: msg.channelId,
    content: msg.content,
    timestamp: msg.createdAt.toISOString(),
    author: { id: msg.author.id, username: msg.author.username, bot: msg.author.bot },
    mentions: msg.mentions.users.map((u) => ({ id: u.id, username: u.username })),
  });
  if (!normalized) return;
  try {
    routeForumPost(normalized, guildId, deps.router, { isStarter: false });
  } catch (err) {
    console.warn(`  discord-forum: reply route failed: ${(err as Error).message}`);
  }
}

export interface FinalizeForumPostArgs {
  /** 議論 session の scene ("discord:<guild>/<threadId>")。フォーラム以外は no-op。 */
  scene: string | null;
  /** 収束まとめ本文 (【収束】プレフィックス除去済)。 */
  summary: string;
  /** 議題タイトル。 */
  title: string;
  /** まとめ転記先チャンネル名 (既定「まとめ投稿」)。 */
  summaryChannelName?: string;
  /** 自動作成カテゴリ名 (既定「システム」)。 */
  categoryName?: string;
}

/**
 * 収束したフォーラムポストを締める: まとめを「まとめ投稿」チャンネルへ転記し、
 * スレッドを lock + archive してクローズする。
 * scene がフォーラムスレッドでない (通常チャンネル等) 場合は何もしない。
 */
export async function finalizeForumPost(
  client: Client,
  args: FinalizeForumPostArgs
): Promise<{ closed: boolean; reason?: string }> {
  const parsed = parseDiscordScene(args.scene);
  if (!parsed) return { closed: false, reason: "scene is not discord-bound" };

  let channel;
  try {
    channel = await client.channels.fetch(parsed.channelId);
  } catch (err) {
    return { closed: false, reason: `channel fetch failed: ${(err as Error).message}` };
  }
  if (!channel || !isForumThreadChannel(channel)) {
    return { closed: false, reason: "not a forum thread" };
  }
  const thread = channel as AnyThreadChannel;

  // 1) まとめを「まとめ投稿」チャンネルへ転記 (権限が無ければ skip)。
  try {
    const summaryChannel = await ensureSystemChannel(client, {
      guildId: parsed.guildId,
      categoryName: args.categoryName,
      channelName: args.summaryChannelName ?? "まとめ投稿",
      topic: "収束した議論の結論まとめ (Discutere フォーラム議論)",
    });
    if (summaryChannel) {
      const link = `\nhttps://discord.com/channels/${parsed.guildId}/${thread.id}`;
      await summaryChannel.send(formatForumSummary(args.title, args.summary) + link).catch(() => {});
    }
  } catch {
    /* まとめ転記の失敗はクローズを止めない */
  }

  // 2) ポストをクローズ (lock → archive)。
  try {
    if (thread.manageable) {
      await thread.setLocked(true, "discussion converged").catch(() => {});
    }
    await thread.setArchived(true, "discussion converged");
    return { closed: true };
  } catch (err) {
    return { closed: false, reason: `archive failed: ${(err as Error).message}` };
  }
}

async function fetchStarterWithRetry(thread: AnyThreadChannel): Promise<Message | null> {
  for (let attempt = 0; attempt < 2; attempt++) {
    try {
      const m = await thread.fetchStarterMessage();
      if (m) return m;
    } catch {
      /* 作成直後は未確定なことがある → リトライ */
    }
    await delay(500);
  }
  return null;
}

function delay(ms: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, ms));
}
