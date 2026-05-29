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
