/**
 * Slack Web API 薄クライアント (fetch 直叩き)。
 *
 * @slack/* SDK は使わず global fetch のみで実装する。Socket Mode の接続確立
 * (apps.connections.open) と、議論発話の投稿 (chat.postMessage) / 投票リアクション
 * (reactions.add) に必要な最小 API だけを持つ。
 *
 * 認証は 2 種類のトークンを使い分ける:
 *   - app-level token (xapp-) : Socket Mode の wss URL 取得 (apps.connections.open)
 *   - bot token (xoxb-)       : Web API (chat.postMessage / reactions.add)
 */

const SLACK_API = "https://slack.com/api";

/** Slack 本文の上限ガード (Block Kit / text の安全側に切る)。 */
const SLACK_TEXT_MAX = 3000;

/** Slack 本文を上限で切り詰める (末尾を … に置換)。 */
export function truncateSlack(s: string, max: number = SLACK_TEXT_MAX): string {
  if (s.length <= max) return s;
  return s.slice(0, max - 1) + "…";
}

/** Slack API の共通レスポンス形 ({ok, error?})。 */
interface SlackResponse {
  ok: boolean;
  error?: string;
  [k: string]: unknown;
}

/** Slack Block Kit のブロック (最小型。各 builder が返す)。 */
export type SlackBlock = Record<string, unknown>;

/** 共通 POST ヘルパ。Bearer トークン + JSON body で叩き、JSON を返す。 */
async function slackPost(method: string, token: string, body: Record<string, unknown>): Promise<SlackResponse> {
  const res = await fetch(`${SLACK_API}/${method}`, {
    method: "POST",
    headers: {
      Authorization: `Bearer ${token}`,
      "Content-Type": "application/json; charset=utf-8",
    },
    body: JSON.stringify(body),
  });
  const json = (await res.json().catch(() => ({ ok: false, error: "invalid_json" }))) as SlackResponse;
  return json;
}

/**
 * Socket Mode の WebSocket URL を取得する (apps.connections.open)。
 * app-level token (xapp-) が要る。`ok!==true` なら throw。
 */
export async function appsConnectionsOpen(appToken: string): Promise<string> {
  const json = await slackPost("apps.connections.open", appToken, {});
  if (json.ok !== true || typeof json.url !== "string") {
    throw new Error(`apps.connections.open 失敗: ${json.error ?? "unknown"}`);
  }
  return json.url;
}

export interface PostMessageArgs {
  channel: string;
  text: string;
  /** スレッド返信にする場合の親 ts。 */
  threadTs?: string;
  /** 表示名上書き (要 chat:write.customize)。 */
  username?: string;
  /** アイコン絵文字 (要 chat:write.customize)。 */
  iconEmoji?: string;
  /** Block Kit ブロック (任意)。 */
  blocks?: SlackBlock[];
}

/**
 * チャンネル / スレッドへメッセージを投稿する (chat.postMessage)。
 * 返り値の ts は投票リアクション付与先として使う。`ok!==true` なら throw。
 *
 * 注意: username / icon_emoji は bot token に `chat:write.customize` スコープが要る。
 * スコープが無い環境では無視されるため、呼び出し側で本文先頭に表示名を出すこと。
 */
export async function postMessage(
  botToken: string,
  args: PostMessageArgs
): Promise<{ ts: string; channel: string }> {
  const body: Record<string, unknown> = {
    channel: args.channel,
    text: truncateSlack(args.text),
  };
  if (args.threadTs) body.thread_ts = args.threadTs;
  if (args.username) body.username = args.username;
  if (args.iconEmoji) body.icon_emoji = args.iconEmoji;
  if (args.blocks) body.blocks = args.blocks;

  const json = await slackPost("chat.postMessage", botToken, body);
  if (json.ok !== true) {
    throw new Error(`chat.postMessage 失敗: ${json.error ?? "unknown"}`);
  }
  return {
    ts: typeof json.ts === "string" ? json.ts : "",
    channel: typeof json.channel === "string" ? json.channel : args.channel,
  };
}

export interface ReactionsAddArgs {
  channel: string;
  /** 対象メッセージの ts。 */
  timestamp: string;
  /** 絵文字名 (例 "trophy" / "+1")。コロン無し。 */
  name: string;
}

/**
 * メッセージに絵文字リアクションを付ける (reactions.add)。
 * `already_reacted` は無視 (throw しない)。それ以外の `ok!==true` は warn のみ。
 */
export async function reactionsAdd(botToken: string, args: ReactionsAddArgs): Promise<void> {
  const json = await slackPost("reactions.add", botToken, {
    channel: args.channel,
    timestamp: args.timestamp,
    name: args.name,
  });
  if (json.ok !== true && json.error !== "already_reacted") {
    console.warn(`  slack-web-api: reactions.add 失敗 (${args.name}): ${json.error ?? "unknown"}`);
  }
}
