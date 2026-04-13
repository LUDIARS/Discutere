/**
 * タスクモードのヒアリング、議論モードの通知等で
 * チャット (Slack / Discord) に投稿を送るヘルパ。
 *
 * - Slack   : chat.postMessage API にスレッド ts を付けて投稿
 * - Discord : channels/{channel.id}/messages にリプライ (message_reference)
 *
 * BOT トークン未設定時は何もせずに成功扱いで返す (ローカル開発向け)。
 * 呼び出し側は monitorId を渡して該当 channel_monitors 行から BOT 認証情報を引く。
 */

import { monitorRepo } from "../db/repository.js";

const HEARING_PROMPTS: Record<string, string> = {
  title: "タイトル (何をするか)",
  assignee: "担当者",
  due: "納期 / 期限",
  context: "背景 / 目的",
  description: "詳細",
};

function buildHearingText(missing: string[]): string {
  const unique = Array.from(new Set(missing));
  const asked = unique
    .map((f) => HEARING_PROMPTS[f] ?? f)
    .filter(Boolean)
    .join("、");
  return `タスクとして登録しようと思いますが、以下の情報が不足しています: ${asked || "(詳細)"}`;
}

export async function sendHearingReply(args: {
  monitorId: string;
  platform: "slack" | "discord";
  channelId: string;
  threadKey: string;
  missingFields: string[];
}): Promise<void> {
  const text = buildHearingText(args.missingFields);
  await sendChatReply({
    monitorId: args.monitorId,
    platform: args.platform,
    channelId: args.channelId,
    threadKey: args.threadKey,
    text,
  });
}

export async function sendChatReply(args: {
  monitorId: string;
  platform: "slack" | "discord";
  channelId: string;
  threadKey: string;
  text: string;
}): Promise<void> {
  const monitor = await monitorRepo.findById(args.monitorId);
  const token = monitor?.botToken ?? null;

  if (!token) {
    console.log(
      `[chat-reply:${args.platform}] BOT トークン未設定のためスキップ: "${args.text}"`
    );
    return;
  }

  if (args.platform === "slack") {
    await postSlack(token, args.channelId, args.threadKey, args.text);
  } else {
    await postDiscord(token, args.channelId, args.threadKey, args.text);
  }
}

async function postSlack(
  botToken: string,
  channelId: string,
  threadTs: string,
  text: string
): Promise<void> {
  const res = await fetch("https://slack.com/api/chat.postMessage", {
    method: "POST",
    headers: {
      Authorization: `Bearer ${botToken}`,
      "Content-Type": "application/json; charset=utf-8",
    },
    body: JSON.stringify({
      channel: channelId,
      thread_ts: threadTs,
      text,
    }),
  });
  if (!res.ok) {
    throw new Error(`Slack chat.postMessage HTTP ${res.status}`);
  }
  const data = (await res.json()) as { ok: boolean; error?: string };
  if (!data.ok) {
    throw new Error(`Slack chat.postMessage error: ${data.error ?? "unknown"}`);
  }
}

async function postDiscord(
  botToken: string,
  channelId: string,
  messageId: string,
  text: string
): Promise<void> {
  const res = await fetch(
    `https://discord.com/api/v10/channels/${encodeURIComponent(channelId)}/messages`,
    {
      method: "POST",
      headers: {
        Authorization: `Bot ${botToken}`,
        "Content-Type": "application/json",
      },
      body: JSON.stringify({
        content: text,
        message_reference: { message_id: messageId },
        allowed_mentions: { replied_user: true },
      }),
    }
  );
  if (!res.ok) {
    const body = await res.text();
    throw new Error(`Discord POST message HTTP ${res.status}: ${body}`);
  }
}
