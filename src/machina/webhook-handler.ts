/**
 * M3 MACHINA: Webhook受信ハンドラ
 *
 * Slack/Discord からの Incoming Webhook を受け取り、
 * メッセージを解析してタスクの自動生成/更新を行う。
 */

import { randomUUID } from "crypto";
import { analyzeMessage } from "./analyzer.js";
import {
  taskRepo,
  taskLogRepo,
  monitorRepo,
  userRepo,
  chatMessageRepo,
} from "../db/repository.js";
import { relayTaskToPm } from "./pm-relay.js";

/** Slack Event APIのメッセージ形式 */
interface SlackMessageEvent {
  type: string;
  subtype?: string;
  channel: string;
  user: string;
  text: string;
  ts: string;
  thread_ts?: string;
}

/** Discord Webhook のメッセージ形式 */
interface DiscordMessageEvent {
  id: string;
  channel_id: string;
  author: {
    id: string;
    username: string;
  };
  content: string;
  mentions: Array<{ id: string; username: string }>;
  timestamp: string;
}

/** 共通メッセージ形式 */
interface NormalizedMessage {
  platform: "slack" | "discord";
  channelId: string;
  authorId: string;
  authorName: string;
  text: string;
  messageId: string;
  mentions: string[];
}

/**
 * Slack メッセージイベントを処理
 */
export async function handleSlackMessage(
  event: SlackMessageEvent,
  workspaceId: string
): Promise<Record<string, unknown> | null> {
  // bot メッセージやサブタイプ付きメッセージは無視
  if (event.subtype) return null;

  const normalized: NormalizedMessage = {
    platform: "slack",
    channelId: event.channel,
    authorId: event.user,
    authorName: event.user,
    text: event.text,
    messageId: event.ts,
    mentions: extractSlackMentions(event.text),
  };

  return processMessage(normalized, workspaceId);
}

/**
 * Discord メッセージイベントを処理
 */
export async function handleDiscordMessage(
  event: DiscordMessageEvent,
  workspaceId: string
): Promise<Record<string, unknown> | null> {
  const normalized: NormalizedMessage = {
    platform: "discord",
    channelId: event.channel_id,
    authorId: event.author.id,
    authorName: event.author.username,
    text: event.content,
    messageId: event.id,
    mentions: event.mentions.map((m) => m.username),
  };

  return processMessage(normalized, workspaceId);
}

/**
 * 正規化されたメッセージを処理し、タスクを生成/更新する
 */
async function processMessage(
  msg: NormalizedMessage,
  workspaceId: string
): Promise<Record<string, unknown> | null> {
  // チャンネルが監視対象か確認
  const monitors = await monitorRepo.findActiveByWorkspaceId(workspaceId);
  const monitor = monitors.find(
    (m) => m.platform === msg.platform && m.channelId === msg.channelId
  );
  if (!monitor) return null;

  // チャットログを保存 (captureMessages が有効な場合)
  if (monitor.captureMessages) {
    try {
      await chatMessageRepo.create({
        id: randomUUID(),
        monitorId: monitor.id,
        workspaceId,
        platform: msg.platform,
        channelId: msg.channelId,
        messageId: msg.messageId,
        authorId: msg.authorId,
        authorName: msg.authorName,
        text: msg.text,
        meta: JSON.stringify({ mentions: msg.mentions }),
        postedAt: new Date(),
        createdAt: new Date(),
      });
    } catch (err) {
      console.error("[machina:webhook] chat log 保存エラー:", err);
    }
  }

  // テキストを解析
  const analysis = analyzeMessage({
    text: msg.text,
    authorId: msg.authorId,
    authorName: msg.authorName,
    mentions: msg.mentions,
    platform: msg.platform,
  });

  // タスク更新（完了キーワード検出時）
  if (analysis.shouldUpdateExisting && analysis.isCompletion) {
    await handleTaskCompletion(msg, workspaceId);
    return null;
  }

  // タスク生成
  if (!analysis.shouldCreateTask) return null;
  if (analysis.confidence < 0.5) return null;

  // アサイン先の解決
  const assigneeId = await resolveAssignee(
    analysis.assigneeHint,
    workspaceId
  );

  const taskId = randomUUID();
  const now = new Date();

  const taskData = {
    id: taskId,
    workspaceId,
    title: analysis.title,
    description: analysis.description,
    status: "pending",
    priority: analysis.priority,
    assigneeId,
    dueDate: analysis.dueDateHint,
    source: "auto",
    sourcePlatform: msg.platform,
    sourceMessageId: msg.messageId,
    sourceChannelId: msg.channelId,
    sourceText: msg.text.slice(0, 2000),
    confidence: Math.round(analysis.confidence * 100),
    isCriticalPath: false,
    relayedToExternal: false,
    externalTaskId: null,
    createdBy: msg.authorId,
    createdAt: now,
    updatedAt: now,
  };

  await taskRepo.create(taskData);

  // ログ記録
  await taskLogRepo.create({
    id: randomUUID(),
    taskId,
    action: "created",
    previousValue: null,
    newValue: JSON.stringify({ title: analysis.title, priority: analysis.priority }),
    reason: analysis.reasoning,
    triggerMessageId: msg.messageId,
    performedBy: "system",
    createdAt: now,
  });

  // TODO: notification events via Cernere relay

  // PM リレー
  const relayResult = await relayTaskToPm(taskData);
  if (relayResult) {
    await taskRepo.update(taskId, {
      relayedToExternal: true,
      externalTaskId: relayResult.externalTaskId,
    });
    await taskLogRepo.create({
      id: randomUUID(),
      taskId,
      action: "relayed",
      previousValue: null,
      newValue: JSON.stringify({ externalTaskId: relayResult.externalTaskId }),
      reason: "PM (M2) へ自動リレー",
      triggerMessageId: null,
      performedBy: "system",
      createdAt: new Date(),
    });
  }

  return taskData;
}

/**
 * 完了キーワードが検出された場合、関連タスクを更新
 */
async function handleTaskCompletion(
  msg: NormalizedMessage,
  workspaceId: string
): Promise<void> {
  // 進行中・未着手のタスクから、メッセージ作者にアサインされたものを検索
  const inProgressTasks = await taskRepo.findByWorkspaceIdAndStatus(
    workspaceId,
    "in_progress"
  );
  const pendingTasks = await taskRepo.findByWorkspaceIdAndStatus(
    workspaceId,
    "pending"
  );
  const activeTasks = [...inProgressTasks, ...pendingTasks];

  // メンションされたユーザーまたは作者のタスクを完了にする
  const resolvedAuthor = await resolveAssignee(msg.authorName, workspaceId);
  const targetTasks = activeTasks.filter(
    (t) => t.assigneeId === resolvedAuthor || t.assigneeId === msg.authorId
  );

  for (const task of targetTasks) {
    const prevStatus = task.status;
    await taskRepo.update(task.id, { status: "done" });
    await taskLogRepo.create({
      id: randomUUID(),
      taskId: task.id,
      action: "status_changed",
      previousValue: JSON.stringify({ status: prevStatus }),
      newValue: JSON.stringify({ status: "done" }),
      reason: `完了キーワード検出: "${msg.text.slice(0, 100)}"`,
      triggerMessageId: msg.messageId,
      performedBy: "system",
      createdAt: new Date(),
    });

    // TODO: notification events via Cernere relay
  }
}

/**
 * アサインヒントからユーザーIDを解決する
 */
async function resolveAssignee(
  hint: string | null,
  workspaceId: string
): Promise<string | null> {
  if (!hint) return null;

  // TODO: resolve workspace members via Cernere
  const userIds: string[] = [];

  // ユーザー名で検索
  for (const userId of userIds) {
    const user = await userRepo.findById(userId);
    if (!user) continue;
    if (
      user.displayName === hint ||
      user.displayName.toLowerCase() === hint.toLowerCase() ||
      (user.email ?? "").split("@")[0] === hint
    ) {
      return user.id;
    }
  }

  return null;
}

/**
 * Slack テキストからメンションを抽出
 * フォーマット: <@U12345>
 */
function extractSlackMentions(text: string): string[] {
  const matches = text.matchAll(/<@(\w+)>/g);
  return Array.from(matches, (m) => m[1]);
}
