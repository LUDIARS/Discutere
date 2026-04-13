/**
 * チャンネルモード「タスク」の処理器。
 *
 * フロー:
 *   1. 投稿を受信 → 既存のスレッドセッションがあれば追記 (collecting)
 *   2. なければ新規セッションを作成し Haiku で分類
 *   3. タスクっぽいが情報不足 → hearing 状態にしてチャットへヒアリング投稿
 *   4. タスクっぽくて十分 → タスクを登録 (DB)、セッション終了
 *   5. 登録中に新着投稿 → 追加情報として DB タスクの description へ追記
 *
 * DB は「最終的に登録されたタスク」だけを保存し、
 * 処理途中のセッションはオンメモリ (mode-state.ts) に保持する。
 */

import { randomUUID } from "crypto";
import { taskRepo, taskLogRepo } from "../db/repository.js";
import { classifyMessage } from "./haiku-classifier.js";
import {
  taskSessionStore,
  type TaskSession,
  type TaskModeMessage,
} from "./mode-state.js";
import type { TaskPriority } from "../shared/constants.js";
import { sendHearingReply } from "./chat-reply.js";

export interface TaskModeInput {
  monitorId: string;
  workspaceId: string;
  platform: "slack" | "discord";
  channelId: string;
  messageId: string;
  /** Slack なら thread_ts (無ければ messageId)、Discord なら messageId */
  threadKey: string;
  authorId: string;
  authorName: string;
  text: string;
  postedAt: Date;
}

/** タスクモードでメッセージを処理する */
export async function handleTaskModeMessage(input: TaskModeInput): Promise<{
  sessionId: string;
  action: "appended" | "hearing" | "registered" | "skipped";
} | null> {
  // 既存セッション (同スレッド) があれば追記扱い
  const existing = taskSessionStore.findActiveByThread(
    input.monitorId,
    input.threadKey
  );

  const newMsg: TaskModeMessage = {
    authorId: input.authorId,
    authorName: input.authorName,
    text: input.text,
    messageId: input.messageId,
    postedAt: input.postedAt,
  };

  if (existing) {
    return appendToExisting(existing, input, newMsg);
  }

  // 新規セッション
  const session = taskSessionStore.create({
    monitorId: input.monitorId,
    workspaceId: input.workspaceId,
    platform: input.platform,
    channelId: input.channelId,
    threadKey: input.threadKey,
    status: "classifying",
    messages: [newMsg],
    classification: null,
  });

  try {
    const classification = await classifyMessage({
      text: input.text,
      platform: input.platform,
    });
    taskSessionStore.update(session.id, { classification });

    if (!classification.isTask || classification.confidence < 0.4) {
      // タスクではない → セッション破棄
      taskSessionStore.remove(session.id);
      return { sessionId: session.id, action: "skipped" };
    }

    if (classification.missingFields.length > 0) {
      taskSessionStore.update(session.id, { status: "hearing" });
      await safeHearing(session, classification.missingFields);
      return { sessionId: session.id, action: "hearing" };
    }

    // 情報が揃っている → タスク登録して終了
    const taskId = await registerTask(session, input, classification);
    taskSessionStore.update(session.id, {
      status: "registering",
      taskId,
    });
    taskSessionStore.remove(session.id);
    return { sessionId: session.id, action: "registered" };
  } catch (err) {
    taskSessionStore.update(session.id, {
      status: "failed",
      errorReason: err instanceof Error ? err.message : String(err),
    });
    return { sessionId: session.id, action: "skipped" };
  }
}

/** 既存セッションへの追記処理 */
async function appendToExisting(
  session: TaskSession,
  input: TaskModeInput,
  newMsg: TaskModeMessage
): Promise<{ sessionId: string; action: "appended" | "registered" }> {
  const nextMessages = [...session.messages, newMsg];
  taskSessionStore.update(session.id, { messages: nextMessages });

  // hearing 中なら追加投稿で情報が揃ったかを再評価
  if (session.status === "hearing") {
    const joinedText = nextMessages.map((m) => `${m.authorName}: ${m.text}`).join("\n");
    const classification = await classifyMessage({
      text: joinedText,
      platform: input.platform,
    });
    taskSessionStore.update(session.id, { classification });

    if (classification.isTask && classification.missingFields.length === 0) {
      const taskId = await registerTask(session, input, classification);
      taskSessionStore.update(session.id, { status: "registering", taskId });
      taskSessionStore.remove(session.id);
      return { sessionId: session.id, action: "registered" };
    }
    return { sessionId: session.id, action: "appended" };
  }

  // collecting 中 → 登録済みタスクへの補足として description を追記
  if (session.status === "collecting" && session.taskId) {
    const task = await taskRepo.findById(session.taskId);
    if (task) {
      const nextDesc = (task.description ?? "") + `\n\n(追記) ${input.authorName}: ${input.text}`;
      await taskRepo.update(session.taskId, { description: nextDesc });
      await taskLogRepo.create({
        id: randomUUID(),
        taskId: session.taskId,
        action: "updated",
        previousValue: null,
        newValue: null,
        reason: "タスクモード: 同スレッドからの追記",
        triggerMessageId: input.messageId,
        performedBy: "system",
        createdAt: new Date(),
      });
    }
    return { sessionId: session.id, action: "appended" };
  }

  return { sessionId: session.id, action: "appended" };
}

/** 登録 — DB に書き込み、taskId を返す */
async function registerTask(
  session: TaskSession,
  input: TaskModeInput,
  classification: NonNullable<TaskSession["classification"]>
): Promise<string> {
  const taskId = randomUUID();
  const now = new Date();
  const title = classification.title?.trim() || session.messages[0].text.slice(0, 120);
  const description =
    classification.description?.trim() ||
    session.messages.map((m) => `${m.authorName}: ${m.text}`).join("\n");

  await taskRepo.create({
    id: taskId,
    workspaceId: session.workspaceId,
    title,
    description,
    status: "pending",
    priority: normalizePriority(classification.priority),
    assigneeId: null,
    dueDate: null,
    source: "auto",
    sourcePlatform: session.platform,
    sourceMessageId: session.messages[0].messageId,
    sourceChannelId: session.channelId,
    sourceText: session.messages.map((m) => m.text).join("\n").slice(0, 2000),
    confidence: Math.round(classification.confidence * 100),
    isCriticalPath: false,
    relayedToExternal: false,
    externalTaskId: null,
    createdBy: input.authorId,
    createdAt: now,
    updatedAt: now,
  });

  await taskLogRepo.create({
    id: randomUUID(),
    taskId,
    action: "created",
    previousValue: null,
    newValue: JSON.stringify({ title, mode: "task" }),
    reason: classification.reasoning || "タスクモード自動生成",
    triggerMessageId: session.messages[0].messageId,
    performedBy: "system",
    createdAt: now,
  });

  return taskId;
}

function normalizePriority(raw: string | undefined): TaskPriority {
  if (raw === "low" || raw === "medium" || raw === "high" || raw === "critical") {
    return raw;
  }
  return "medium";
}

/** ヒアリング投稿はエラーを握りつぶす (未設定環境では noop) */
async function safeHearing(session: TaskSession, missing: string[]) {
  try {
    await sendHearingReply({
      monitorId: session.monitorId,
      platform: session.platform,
      channelId: session.channelId,
      threadKey: session.threadKey,
      missingFields: missing,
    });
  } catch (err) {
    console.warn("[task-mode] ヒアリング投稿に失敗:", err);
  }
}

// ─── Manual control (called from routes when frontend issues a directive) ───

/** 停滞セッションに対してユーザーが任意の補足文を与え、再評価する */
export async function resumeSession(
  sessionId: string,
  supplementText: string,
  performedBy: string
): Promise<{ action: "registered" | "still_hearing" | "not_found" }> {
  const session = taskSessionStore.findById(sessionId);
  if (!session) return { action: "not_found" };

  const suppMsg: TaskModeMessage = {
    authorId: performedBy,
    authorName: performedBy,
    text: supplementText,
    messageId: `manual-${Date.now()}`,
    postedAt: new Date(),
  };
  const nextMessages = [...session.messages, suppMsg];

  const joined = nextMessages.map((m) => `${m.authorName}: ${m.text}`).join("\n");
  const classification = await classifyMessage({
    text: joined,
    platform: session.platform,
  });

  taskSessionStore.update(sessionId, { messages: nextMessages, classification });

  if (classification.isTask && classification.missingFields.length === 0) {
    const taskId = await registerTask(session, {
      monitorId: session.monitorId,
      workspaceId: session.workspaceId,
      platform: session.platform,
      channelId: session.channelId,
      messageId: session.messages[0].messageId,
      threadKey: session.threadKey,
      authorId: performedBy,
      authorName: performedBy,
      text: supplementText,
      postedAt: new Date(),
    }, classification);
    taskSessionStore.update(sessionId, { status: "registering", taskId });
    taskSessionStore.remove(sessionId);
    return { action: "registered" };
  }

  taskSessionStore.update(sessionId, { status: "hearing" });
  return { action: "still_hearing" };
}

/** 停滞セッションを強制的に破棄 (対応不要判定) */
export function dismissSession(sessionId: string): boolean {
  return taskSessionStore.remove(sessionId);
}
