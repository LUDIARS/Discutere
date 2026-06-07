/**
 * Discord channel への message 投稿 (PR-I).
 *
 * - Bot Token で `POST /channels/{id}/messages` を直叩き
 * - 失敗は throw (caller が catch & log する想定)
 * - persona-engine の AI 発話を Discord 議論 channel に流すための最小 API
 */

const DISCORD_API = "https://discord.com/api/v10";

export interface DiscordPostArgs {
  botToken: string;
  channelId: string;
  content: string;
  /** 返信元 message id (任意) */
  replyTo?: string;
  /** ephemeral 風表示は通常 message では出せない、 interaction reply で扱う */
}

export async function postDiscordChannel(args: DiscordPostArgs): Promise<{ id: string }> {
  const body: Record<string, unknown> = { content: truncate(args.content, 1900) };
  if (args.replyTo) {
    body.message_reference = { message_id: args.replyTo };
    body.allowed_mentions = { replied_user: false };
  }
  const res = await fetch(
    `${DISCORD_API}/channels/${encodeURIComponent(args.channelId)}/messages`,
    {
      method: "POST",
      headers: {
        Authorization: `Bot ${args.botToken}`,
        "Content-Type": "application/json",
      },
      body: JSON.stringify(body),
    }
  );
  if (!res.ok) {
    const text = await res.text().catch(() => "");
    throw new Error(`discord post ${res.status}: ${text.slice(0, 200)}`);
  }
  const json = (await res.json()) as { id?: string };
  return { id: json.id ?? "" };
}

function truncate(s: string, max: number): string {
  if (s.length <= max) return s;
  return s.slice(0, max - 1) + "…";
}

// ───────── Webhook (persona を人間名で投稿するため) ─────────

const WEBHOOK_NAME = "Discutere議論";
const webhookCache = new Map<string, { id: string; token: string }>();

/** channel に議論用 webhook を確保 (なければ作成)。 Manage Webhooks 権限が要る。 */
export async function ensureChannelWebhook(
  botToken: string,
  channelId: string
): Promise<{ id: string; token: string }> {
  const cached = webhookCache.get(channelId);
  if (cached) return cached;

  const list = await fetch(`${DISCORD_API}/channels/${encodeURIComponent(channelId)}/webhooks`, {
    headers: { Authorization: `Bot ${botToken}` },
  });
  if (list.ok) {
    const hooks = (await list.json()) as Array<{ id: string; token?: string; name: string }>;
    const mine = hooks.find((h) => h.token && h.name === WEBHOOK_NAME);
    if (mine?.token) {
      const v = { id: mine.id, token: mine.token };
      webhookCache.set(channelId, v);
      return v;
    }
  }
  const res = await fetch(`${DISCORD_API}/channels/${encodeURIComponent(channelId)}/webhooks`, {
    method: "POST",
    headers: { Authorization: `Bot ${botToken}`, "Content-Type": "application/json" },
    body: JSON.stringify({ name: WEBHOOK_NAME }),
  });
  if (!res.ok) {
    throw new Error(`create webhook ${res.status}: ${(await res.text().catch(() => "")).slice(0, 200)}`);
  }
  const j = (await res.json()) as { id: string; token: string };
  const v = { id: j.id, token: j.token };
  webhookCache.set(channelId, v);
  return v;
}

/**
 * webhook 投稿先の解決。 フォーラムスレッド (= channelId が thread) は webhook を
 * 持てないので、 親フォーラムチャンネルの webhook を使い thread_id でスレッドに流す。
 * 通常チャンネルはそのまま。 解決に失敗したら channelId をそのまま使う (caller が fallback)。
 */
export async function resolveWebhookTarget(
  botToken: string,
  channelId: string
): Promise<{ webhookChannelId: string; threadId?: string }> {
  try {
    const res = await fetch(`${DISCORD_API}/channels/${encodeURIComponent(channelId)}`, {
      headers: { Authorization: `Bot ${botToken}` },
    });
    if (!res.ok) return { webhookChannelId: channelId };
    const ch = (await res.json()) as { type?: number; parent_id?: string | null };
    // 10=announcement thread, 11=public thread, 12=private thread
    if ((ch.type === 10 || ch.type === 11 || ch.type === 12) && ch.parent_id) {
      return { webhookChannelId: ch.parent_id, threadId: channelId };
    }
    return { webhookChannelId: channelId };
  } catch {
    return { webhookChannelId: channelId };
  }
}

export interface DiscordWebhookPostArgs {
  webhookId: string;
  webhookToken: string;
  username: string;
  content: string;
  avatarUrl?: string;
  /** フォーラムスレッドに流す場合の thread id (親チャンネルの webhook 経由)。 */
  threadId?: string;
}

/** webhook で username (= persona の人間名) を変えて投稿。 message id を返す。 */
export async function postDiscordWebhook(args: DiscordWebhookPostArgs): Promise<{ id: string }> {
  const threadQuery = args.threadId ? `&thread_id=${encodeURIComponent(args.threadId)}` : "";
  const res = await fetch(
    `${DISCORD_API}/webhooks/${args.webhookId}/${args.webhookToken}?wait=true${threadQuery}`,
    {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        content: truncate(args.content, 1900),
        username: args.username.slice(0, 80),
        avatar_url: args.avatarUrl,
      }),
    }
  );
  if (!res.ok) {
    throw new Error(`webhook post ${res.status}: ${(await res.text().catch(() => "")).slice(0, 200)}`);
  }
  const json = (await res.json().catch(() => ({}))) as { id?: string };
  return { id: json.id ?? "" };
}

/**
 * 人間のチャットらしく整形する:
 *  - 先頭の機械的ラベル (反証:/反例:/弱点: 等) を除去
 *  - 文末 (。！？) ごとに改行
 */
export function humanizeForDiscord(text: string): string {
  let t = text.trim();
  t = t.replace(/^(反証|反例|弱点|指摘|反論|懸念)\s*[:：]\s*/u, "");
  t = t.replace(/([。！？!?])(?!\n|$)/gu, "$1\n");
  return t;
}
