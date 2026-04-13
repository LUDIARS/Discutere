/**
 * M3 MACHINA: タスク自動生成モジュール ルート
 *
 * Slack/Discord のログを監視し、タスクを自動生成/自動更新する。
 * グループに属する形でタスクを管理。
 *
 * エンドポイント:
 *   - GET    /groups/:workspaceId/tasks          — タスク一覧
 *   - GET    /groups/:workspaceId/tasks/:taskId   — タスク詳細
 *   - POST   /groups/:workspaceId/tasks          — タスク手動作成
 *   - PUT    /groups/:workspaceId/tasks/:taskId   — タスク更新
 *   - DELETE /groups/:workspaceId/tasks/:taskId   — タスク削除
 *   - GET    /groups/:workspaceId/monitors       — チャンネル監視一覧
 *   - POST   /groups/:workspaceId/monitors       — チャンネル監視追加
 *   - PUT    /groups/:workspaceId/monitors/:id   — チャンネル監視更新
 *   - DELETE /groups/:workspaceId/monitors/:id   — チャンネル監視削除
 *   - POST   /groups/:workspaceId/tasks/:taskId/relay — PM (M2) へリレー
 *   - GET    /groups/:workspaceId/tasks/:taskId/logs  — タスクログ一覧
 *   - POST   /webhook/slack                  — Slack Incoming Webhook
 *   - POST   /webhook/discord                — Discord Incoming Webhook
 *   - POST   /analyze                        — テキスト解析 (プレビュー)
 */

import { Hono } from "hono";
import { randomUUID } from "crypto";
import { getUserId, getUserRole } from "../middleware/auth.js";
import {
  taskRepo,
  taskLogRepo,
  monitorRepo,
  chatMessageRepo,
  chatSummaryRepo,
} from "../db/repository.js";
import { analyzeMessage } from "./analyzer.js";
import { handleSlackMessage, handleDiscordMessage } from "./webhook-handler.js";
import { relayTaskToPm, relayTaskUpdateToPm, hasPmRelay } from "./pm-relay.js";
import { summarizeMessages } from "./summarizer.js";
import {
  taskSessionStore,
  discussionSessionStore,
  serializeTaskSession,
  serializeDiscussionSession,
} from "./mode-state.js";
import { resumeSession, dismissSession } from "./task-mode.js";
import {
  dismissDiscussionSession,
  flushDiscussionSession,
} from "./discussion-mode.js";
import { CHANNEL_MODES, type ChannelMode } from "../shared/constants.js";
// logActivity removed

/** logActivity wrapper: userName を "MACHINA" に固定 */
function logMachina(userId: string, action: string, detail: string): void {
  console.log(`[MACHINA] ${action}: ${detail}`);
}
import {
  TASK_STATUSES,
  TASK_PRIORITIES,
} from "../shared/constants.js";
import type { TaskStatus, TaskPriority } from "../shared/constants.js";

export const machinaRoutes = new Hono();

// ─── Helpers ──────────────────────────────────────────────────

async function checkGroupAccess(
  userId: string,
  workspaceId: string,
  systemRole: string
): Promise<boolean> {
  if (systemRole === "admin") return true;
  const memberships = await Promise.resolve([]);
  return memberships.some(
    (m: { workspaceId: string }) => m.workspaceId === workspaceId
  );
}

// ─── Tasks CRUD ───────────────────────────────────────────────

// GET /groups/:workspaceId/tasks — タスク一覧
machinaRoutes.get("/groups/:workspaceId/tasks", async (c) => {
  const userId = getUserId(c);
  if (!userId) return c.json({ error: "Authentication required" }, 401);
  const systemRole = getUserRole(c);
  const workspaceId = c.req.param("workspaceId");

  if (!(await checkGroupAccess(userId, workspaceId, systemRole))) {
    return c.json({ error: "このグループへのアクセス権がありません" }, 403);
  }

  const status = c.req.query("status");
  const tasks = status
    ? await taskRepo.findByWorkspaceIdAndStatus(workspaceId, status)
    : await taskRepo.findByWorkspaceId(workspaceId);

  // アサイン先のユーザー名を付与
  const userIds = [...new Set(tasks.map((t) => t.assigneeId).filter(Boolean))] as string[];
  const userMap = new Map<string, string>();
  for (const uid of userIds) {
    const user = await import("../db/repository.js").then(m => m.userRepo.findById(uid));
    if (user) userMap.set(uid, user.displayName);
  }

  const enrichedTasks = tasks.map((t) => ({
    ...t,
    assigneeName: t.assigneeId ? userMap.get(t.assigneeId) ?? null : null,
  }));

  return c.json({ tasks: enrichedTasks });
});

// GET /groups/:workspaceId/tasks/:taskId — タスク詳細
machinaRoutes.get("/groups/:workspaceId/tasks/:taskId", async (c) => {
  const userId = getUserId(c);
  if (!userId) return c.json({ error: "Authentication required" }, 401);
  const systemRole = getUserRole(c);
  const workspaceId = c.req.param("workspaceId");
  const taskId = c.req.param("taskId");

  if (!(await checkGroupAccess(userId, workspaceId, systemRole))) {
    return c.json({ error: "このグループへのアクセス権がありません" }, 403);
  }

  const task = await taskRepo.findById(taskId);
  if (!task || task.workspaceId !== workspaceId) {
    return c.json({ error: "タスクが見つかりません" }, 404);
  }

  let assigneeName: string | null = null;
  if (task.assigneeId) {
    const { userRepo: uRepo } = await import("../db/repository.js");
    const user = await uRepo.findById(task.assigneeId);
    assigneeName = user?.displayName ?? null;
  }

  const logs = await taskLogRepo.findByTaskId(taskId);

  return c.json({ task: { ...task, assigneeName }, logs });
});

// POST /groups/:workspaceId/tasks — タスク手動作成
machinaRoutes.post("/groups/:workspaceId/tasks", async (c) => {
  const userId = getUserId(c);
  if (!userId) return c.json({ error: "Authentication required" }, 401);
  const systemRole = getUserRole(c);
  const workspaceId = c.req.param("workspaceId");

  if (!(await checkGroupAccess(userId, workspaceId, systemRole))) {
    return c.json({ error: "このグループへのアクセス権がありません" }, 403);
  }

  const body = await c.req.json<{
    title: string;
    description?: string;
    priority?: TaskPriority;
    assigneeId?: string;
    dueDate?: string;
  }>();

  if (!body.title || body.title.trim().length === 0) {
    return c.json({ error: "タイトルは必須です" }, 400);
  }

  if (body.priority && !TASK_PRIORITIES.includes(body.priority)) {
    return c.json({ error: `優先度は ${TASK_PRIORITIES.join("/")} のいずれかです` }, 400);
  }

  const taskId = randomUUID();
  const now = new Date();

  await taskRepo.create({
    id: taskId,
    workspaceId,
    title: body.title.trim(),
    description: body.description ?? null,
    status: "pending",
    priority: body.priority ?? "medium",
    assigneeId: body.assigneeId ?? null,
    dueDate: body.dueDate ?? null,
    source: "manual",
    sourcePlatform: null,
    sourceMessageId: null,
    sourceChannelId: null,
    sourceText: null,
    confidence: 100,
    isCriticalPath: false,
    relayedToExternal: false,
    externalTaskId: null,
    createdBy: userId,
    createdAt: now,
    updatedAt: now,
  });

  await taskLogRepo.create({
    id: randomUUID(),
    taskId,
    action: "created",
    previousValue: null,
    newValue: JSON.stringify({ title: body.title, priority: body.priority ?? "medium" }),
    reason: "手動作成",
    triggerMessageId: null,
    performedBy: userId,
    createdAt: now,
  });

  logMachina(userId, "task_created", `タスク「${body.title}」を作成`);

  return c.json({ id: taskId, message: "タスクを作成しました" }, 201);
});

// PUT /groups/:workspaceId/tasks/:taskId — タスク更新
machinaRoutes.put("/groups/:workspaceId/tasks/:taskId", async (c) => {
  const userId = getUserId(c);
  if (!userId) return c.json({ error: "Authentication required" }, 401);
  const systemRole = getUserRole(c);
  const workspaceId = c.req.param("workspaceId");
  const taskId = c.req.param("taskId");

  if (!(await checkGroupAccess(userId, workspaceId, systemRole))) {
    return c.json({ error: "このグループへのアクセス権がありません" }, 403);
  }

  const task = await taskRepo.findById(taskId);
  if (!task || task.workspaceId !== workspaceId) {
    return c.json({ error: "タスクが見つかりません" }, 404);
  }

  const body = await c.req.json<{
    title?: string;
    description?: string;
    status?: TaskStatus;
    priority?: TaskPriority;
    assigneeId?: string | null;
    dueDate?: string | null;
    isCriticalPath?: boolean;
  }>();

  if (body.status && !TASK_STATUSES.includes(body.status)) {
    return c.json({ error: `ステータスは ${TASK_STATUSES.join("/")} のいずれかです` }, 400);
  }
  if (body.priority && !TASK_PRIORITIES.includes(body.priority)) {
    return c.json({ error: `優先度は ${TASK_PRIORITIES.join("/")} のいずれかです` }, 400);
  }

  const updates: Record<string, unknown> = {};
  if (body.title !== undefined) updates.title = body.title;
  if (body.description !== undefined) updates.description = body.description;
  if (body.status !== undefined) updates.status = body.status;
  if (body.priority !== undefined) updates.priority = body.priority;
  if (body.assigneeId !== undefined) updates.assigneeId = body.assigneeId;
  if (body.dueDate !== undefined) updates.dueDate = body.dueDate;
  if (body.isCriticalPath !== undefined) updates.isCriticalPath = body.isCriticalPath;

  await taskRepo.update(taskId, updates);

  // ステータス変更ログ
  if (body.status && body.status !== task.status) {
    await taskLogRepo.create({
      id: randomUUID(),
      taskId,
      action: "status_changed",
      previousValue: JSON.stringify({ status: task.status }),
      newValue: JSON.stringify({ status: body.status }),
      reason: null,
      triggerMessageId: null,
      performedBy: userId,
      createdAt: new Date(),
    });
  }

  // アサイン変更ログ
  if (body.assigneeId !== undefined && body.assigneeId !== task.assigneeId) {
    await taskLogRepo.create({
      id: randomUUID(),
      taskId,
      action: "assigned",
      previousValue: JSON.stringify({ assigneeId: task.assigneeId }),
      newValue: JSON.stringify({ assigneeId: body.assigneeId }),
      reason: null,
      triggerMessageId: null,
      performedBy: userId,
      createdAt: new Date(),
    });
  }

  // 優先度変更ログ
  if (body.priority && body.priority !== task.priority) {
    await taskLogRepo.create({
      id: randomUUID(),
      taskId,
      action: "priority_changed",
      previousValue: JSON.stringify({ priority: task.priority }),
      newValue: JSON.stringify({ priority: body.priority }),
      reason: null,
      triggerMessageId: null,
      performedBy: userId,
      createdAt: new Date(),
    });
  }

  // PM リレー（PMタスクIDがある場合は更新を転送）
  if (task.externalTaskId) {
    await relayTaskUpdateToPm(task.externalTaskId, updates);
  }

  logMachina(userId, "task_updated", `タスク「${task.title}」を更新`);

  return c.json({ message: "タスクを更新しました" });
});

// DELETE /groups/:workspaceId/tasks/:taskId — タスク削除
machinaRoutes.delete("/groups/:workspaceId/tasks/:taskId", async (c) => {
  const userId = getUserId(c);
  if (!userId) return c.json({ error: "Authentication required" }, 401);
  const systemRole = getUserRole(c);
  const workspaceId = c.req.param("workspaceId");
  const taskId = c.req.param("taskId");

  if (!(await checkGroupAccess(userId, workspaceId, systemRole))) {
    return c.json({ error: "このグループへのアクセス権がありません" }, 403);
  }

  const task = await taskRepo.findById(taskId);
  if (!task || task.workspaceId !== workspaceId) {
    return c.json({ error: "タスクが見つかりません" }, 404);
  }

  await taskRepo.deleteById(taskId);
  logMachina(userId, "task_deleted", `タスク「${task.title}」を削除`);

  return c.json({ deleted: taskId });
});

// ─── Task Logs ────────────────────────────────────────────────

// GET /groups/:workspaceId/tasks/:taskId/logs — タスクログ一覧
machinaRoutes.get("/groups/:workspaceId/tasks/:taskId/logs", async (c) => {
  const userId = getUserId(c);
  if (!userId) return c.json({ error: "Authentication required" }, 401);
  const systemRole = getUserRole(c);
  const workspaceId = c.req.param("workspaceId");
  const taskId = c.req.param("taskId");

  if (!(await checkGroupAccess(userId, workspaceId, systemRole))) {
    return c.json({ error: "このグループへのアクセス権がありません" }, 403);
  }

  const task = await taskRepo.findById(taskId);
  if (!task || task.workspaceId !== workspaceId) {
    return c.json({ error: "タスクが見つかりません" }, 404);
  }

  const logs = await taskLogRepo.findByTaskId(taskId);
  return c.json({ logs });
});

// ─── PM Relay ─────────────────────────────────────────────────

// POST /groups/:workspaceId/tasks/:taskId/relay — PM (M2) へ手動リレー
machinaRoutes.post("/groups/:workspaceId/tasks/:taskId/relay", async (c) => {
  const userId = getUserId(c);
  if (!userId) return c.json({ error: "Authentication required" }, 401);
  const systemRole = getUserRole(c);
  const workspaceId = c.req.param("workspaceId");
  const taskId = c.req.param("taskId");

  if (!(await checkGroupAccess(userId, workspaceId, systemRole))) {
    return c.json({ error: "このグループへのアクセス権がありません" }, 403);
  }

  const task = await taskRepo.findById(taskId);
  if (!task || task.workspaceId !== workspaceId) {
    return c.json({ error: "タスクが見つかりません" }, 404);
  }

  if (task.relayedToExternal) {
    return c.json({ error: "このタスクは既にPMへリレー済みです", externalTaskId: task.externalTaskId }, 409);
  }

  if (!hasPmRelay()) {
    return c.json({ error: "PMモジュール (M2) が接続されていません" }, 503);
  }

  const result = await relayTaskToPm(task);
  if (!result) {
    return c.json({ error: "PMへのリレーに失敗しました" }, 500);
  }

  await taskRepo.update(taskId, {
    relayedToExternal: true,
    externalTaskId: result.externalTaskId,
  });

  await taskLogRepo.create({
    id: randomUUID(),
    taskId,
    action: "relayed",
    previousValue: null,
    newValue: JSON.stringify({ externalTaskId: result.externalTaskId }),
    reason: "手動リレー",
    triggerMessageId: null,
    performedBy: userId,
    createdAt: new Date(),
  });

  logMachina(userId, "task_relayed", `タスク「${task.title}」をPMへリレー`);

  return c.json({ message: "PMへリレーしました", externalTaskId: result.externalTaskId });
});

// ─── Channel Monitors CRUD ────────────────────────────────────

// GET /groups/:workspaceId/monitors — チャンネル監視一覧
machinaRoutes.get("/groups/:workspaceId/monitors", async (c) => {
  const userId = getUserId(c);
  if (!userId) return c.json({ error: "Authentication required" }, 401);
  const systemRole = getUserRole(c);
  const workspaceId = c.req.param("workspaceId");

  if (!(await checkGroupAccess(userId, workspaceId, systemRole))) {
    return c.json({ error: "このグループへのアクセス権がありません" }, 403);
  }

  const monitors = await monitorRepo.findByWorkspaceId(workspaceId);
  // シークレットはレスポンスに含めず、登録済みフラグのみ返す
  const safeMonitors = monitors.map((m) => ({
    ...m,
    botToken: undefined,
    botSigningSecret: undefined,
    hasBotToken: Boolean(m.botToken),
    hasBotSigningSecret: Boolean(m.botSigningSecret),
    mode: m.mode ?? "task",
    discussionDelayMinutes: m.discussionDelayMinutes ?? 5,
    githubRepo: m.githubRepo ?? null,
    githubDiscussionCategoryId: m.githubDiscussionCategoryId ?? null,
    // 現在処理中のセッション件数 (フロントでのバッジ表示用)
    pendingTaskSessions: taskSessionStore.listByMonitor(m.id).length,
    pendingDiscussionSessions: discussionSessionStore.listByMonitor(m.id).length,
  }));
  return c.json({ monitors: safeMonitors });
});

// POST /groups/:workspaceId/monitors — チャンネル監視追加
machinaRoutes.post("/groups/:workspaceId/monitors", async (c) => {
  const userId = getUserId(c);
  if (!userId) return c.json({ error: "Authentication required" }, 401);
  const systemRole = getUserRole(c);
  const workspaceId = c.req.param("workspaceId");

  if (!(await checkGroupAccess(userId, workspaceId, systemRole))) {
    return c.json({ error: "このグループへのアクセス権がありません" }, 403);
  }

  const body = await c.req.json<{
    platform: string;
    channelId: string;
    channelName: string;
    webhookEndpointId?: string;
    botToken?: string;
    botWorkspaceId?: string;
    botSigningSecret?: string;
    captureMessages?: boolean;
    mode?: ChannelMode;
    discussionDelayMinutes?: number;
    githubRepo?: string;
    githubDiscussionCategoryId?: string;
  }>();

  if (!body.platform || !["slack", "discord"].includes(body.platform)) {
    return c.json({ error: "platform は slack / discord のいずれかです" }, 400);
  }
  if (!body.channelId || !body.channelName) {
    return c.json({ error: "channelId と channelName は必須です" }, 400);
  }
  if (body.mode && !CHANNEL_MODES.includes(body.mode)) {
    return c.json({ error: `mode は ${CHANNEL_MODES.join("/")} のいずれかです` }, 400);
  }

  const id = randomUUID();
  const now = new Date();

  await monitorRepo.create({
    id,
    workspaceId,
    platform: body.platform,
    channelId: body.channelId,
    channelName: body.channelName,
    webhookEndpointId: body.webhookEndpointId ?? null,
    botToken: body.botToken ?? null,
    botWorkspaceId: body.botWorkspaceId ?? null,
    botSigningSecret: body.botSigningSecret ?? null,
    captureMessages: body.captureMessages ?? true,
    mode: body.mode ?? "task",
    discussionDelayMinutes: body.discussionDelayMinutes ?? 5,
    githubRepo: body.githubRepo ?? null,
    githubDiscussionCategoryId: body.githubDiscussionCategoryId ?? null,
    isActive: true,
    createdBy: userId,
    createdAt: now,
    updatedAt: now,
  });

  logMachina(userId, "monitor_created", `チャンネル監視「${body.channelName}」を追加`);

  return c.json({ id, message: "チャンネル監視を追加しました" }, 201);
});

// PUT /groups/:workspaceId/monitors/:id — チャンネル監視更新
machinaRoutes.put("/groups/:workspaceId/monitors/:id", async (c) => {
  const userId = getUserId(c);
  if (!userId) return c.json({ error: "Authentication required" }, 401);
  const systemRole = getUserRole(c);
  const workspaceId = c.req.param("workspaceId");
  const monitorId = c.req.param("id");

  if (!(await checkGroupAccess(userId, workspaceId, systemRole))) {
    return c.json({ error: "このグループへのアクセス権がありません" }, 403);
  }

  const monitor = await monitorRepo.findById(monitorId);
  if (!monitor || monitor.workspaceId !== workspaceId) {
    return c.json({ error: "チャンネル監視が見つかりません" }, 404);
  }

  const body = await c.req.json<{
    channelName?: string;
    isActive?: boolean;
    webhookEndpointId?: string | null;
    botToken?: string | null;
    botWorkspaceId?: string | null;
    botSigningSecret?: string | null;
    captureMessages?: boolean;
    mode?: ChannelMode;
    discussionDelayMinutes?: number;
    githubRepo?: string | null;
    githubDiscussionCategoryId?: string | null;
  }>();

  if (body.mode && !CHANNEL_MODES.includes(body.mode)) {
    return c.json({ error: `mode は ${CHANNEL_MODES.join("/")} のいずれかです` }, 400);
  }

  const updates: Record<string, unknown> = {};
  if (body.channelName !== undefined) updates.channelName = body.channelName;
  if (body.isActive !== undefined) updates.isActive = body.isActive;
  if (body.webhookEndpointId !== undefined) updates.webhookEndpointId = body.webhookEndpointId;
  if (body.botToken !== undefined) updates.botToken = body.botToken;
  if (body.botWorkspaceId !== undefined) updates.botWorkspaceId = body.botWorkspaceId;
  if (body.botSigningSecret !== undefined) updates.botSigningSecret = body.botSigningSecret;
  if (body.captureMessages !== undefined) updates.captureMessages = body.captureMessages;
  if (body.mode !== undefined) updates.mode = body.mode;
  if (body.discussionDelayMinutes !== undefined) updates.discussionDelayMinutes = body.discussionDelayMinutes;
  if (body.githubRepo !== undefined) updates.githubRepo = body.githubRepo;
  if (body.githubDiscussionCategoryId !== undefined)
    updates.githubDiscussionCategoryId = body.githubDiscussionCategoryId;

  await monitorRepo.update(monitorId, updates);

  return c.json({ message: "チャンネル監視を更新しました" });
});

// DELETE /groups/:workspaceId/monitors/:id — チャンネル監視削除
machinaRoutes.delete("/groups/:workspaceId/monitors/:id", async (c) => {
  const userId = getUserId(c);
  if (!userId) return c.json({ error: "Authentication required" }, 401);
  const systemRole = getUserRole(c);
  const workspaceId = c.req.param("workspaceId");
  const monitorId = c.req.param("id");

  if (!(await checkGroupAccess(userId, workspaceId, systemRole))) {
    return c.json({ error: "このグループへのアクセス権がありません" }, 403);
  }

  const monitor = await monitorRepo.findById(monitorId);
  if (!monitor || monitor.workspaceId !== workspaceId) {
    return c.json({ error: "チャンネル監視が見つかりません" }, 404);
  }

  await monitorRepo.deleteById(monitorId);
  logMachina(userId, "monitor_deleted", `チャンネル監視「${monitor.channelName}」を削除`);

  return c.json({ deleted: monitorId });
});

// ─── Chat Messages (Logs) ─────────────────────────────────────

// GET /groups/:workspaceId/monitors/:id/messages — チャットログ取得
machinaRoutes.get("/groups/:workspaceId/monitors/:id/messages", async (c) => {
  const userId = getUserId(c);
  if (!userId) return c.json({ error: "Authentication required" }, 401);
  const systemRole = getUserRole(c);
  const workspaceId = c.req.param("workspaceId");
  const monitorId = c.req.param("id");

  if (!(await checkGroupAccess(userId, workspaceId, systemRole))) {
    return c.json({ error: "このグループへのアクセス権がありません" }, 403);
  }

  const monitor = await monitorRepo.findById(monitorId);
  if (!monitor || monitor.workspaceId !== workspaceId) {
    return c.json({ error: "チャンネル監視が見つかりません" }, 404);
  }

  const limitParam = parseInt(c.req.query("limit") ?? "100", 10);
  const limit = Math.min(500, Math.max(1, isNaN(limitParam) ? 100 : limitParam));

  const messages = await chatMessageRepo.findByMonitorId(monitorId, { limit });
  const total = await chatMessageRepo.countByMonitorId(monitorId);
  return c.json({ messages, total });
});

// ─── Chat Summaries ───────────────────────────────────────────

// GET /groups/:workspaceId/monitors/:id/summaries — 要約一覧
machinaRoutes.get("/groups/:workspaceId/monitors/:id/summaries", async (c) => {
  const userId = getUserId(c);
  if (!userId) return c.json({ error: "Authentication required" }, 401);
  const systemRole = getUserRole(c);
  const workspaceId = c.req.param("workspaceId");
  const monitorId = c.req.param("id");

  if (!(await checkGroupAccess(userId, workspaceId, systemRole))) {
    return c.json({ error: "このグループへのアクセス権がありません" }, 403);
  }

  const monitor = await monitorRepo.findById(monitorId);
  if (!monitor || monitor.workspaceId !== workspaceId) {
    return c.json({ error: "チャンネル監視が見つかりません" }, 404);
  }

  const summaries = await chatSummaryRepo.findByMonitorId(monitorId);
  return c.json({ summaries });
});

// POST /groups/:workspaceId/monitors/:id/summaries — 要約生成
machinaRoutes.post("/groups/:workspaceId/monitors/:id/summaries", async (c) => {
  const userId = getUserId(c);
  if (!userId) return c.json({ error: "Authentication required" }, 401);
  const systemRole = getUserRole(c);
  const workspaceId = c.req.param("workspaceId");
  const monitorId = c.req.param("id");

  if (!(await checkGroupAccess(userId, workspaceId, systemRole))) {
    return c.json({ error: "このグループへのアクセス権がありません" }, 403);
  }

  const monitor = await monitorRepo.findById(monitorId);
  if (!monitor || monitor.workspaceId !== workspaceId) {
    return c.json({ error: "チャンネル監視が見つかりません" }, 404);
  }

  const body = await c.req.json<{
    periodStart?: string; // ISO
    periodEnd?: string; // ISO
    hours?: number; // periodStart/End 未指定時に用いる直近 N 時間
  }>().catch(() => ({} as { periodStart?: string; periodEnd?: string; hours?: number }));

  let periodEnd = body.periodEnd ? new Date(body.periodEnd) : new Date();
  let periodStart: Date;
  if (body.periodStart) {
    periodStart = new Date(body.periodStart);
  } else {
    const hours = body.hours && body.hours > 0 ? body.hours : 24;
    periodStart = new Date(periodEnd.getTime() - hours * 3_600_000);
  }

  if (isNaN(periodStart.getTime()) || isNaN(periodEnd.getTime())) {
    return c.json({ error: "periodStart / periodEnd が不正です" }, 400);
  }
  if (periodStart >= periodEnd) {
    return c.json({ error: "periodStart は periodEnd より前でなければなりません" }, 400);
  }

  const messages = await chatMessageRepo.findByMonitorIdInRange(
    monitorId,
    periodStart,
    periodEnd
  );

  const result = summarizeMessages(messages);

  const summaryId = randomUUID();
  await chatSummaryRepo.create({
    id: summaryId,
    monitorId,
    workspaceId,
    periodStart,
    periodEnd,
    summary: result.summary,
    highlights: JSON.stringify(result.highlights),
    messageCount: result.messageCount,
    createdBy: userId,
    createdAt: new Date(),
  });

  logMachina(
    userId,
    "summary_created",
    `「${monitor.channelName}」の要約を生成 (${result.messageCount} 件)`
  );

  return c.json(
    {
      id: summaryId,
      summary: result.summary,
      highlights: result.highlights,
      messageCount: result.messageCount,
      periodStart: periodStart.toISOString(),
      periodEnd: periodEnd.toISOString(),
    },
    201
  );
});

// DELETE /groups/:workspaceId/monitors/:id/summaries/:summaryId — 要約削除
machinaRoutes.delete(
  "/groups/:workspaceId/monitors/:id/summaries/:summaryId",
  async (c) => {
    const userId = getUserId(c);
    if (!userId) return c.json({ error: "Authentication required" }, 401);
    const systemRole = getUserRole(c);
    const workspaceId = c.req.param("workspaceId");
    const monitorId = c.req.param("id");
    const summaryId = c.req.param("summaryId");

    if (!(await checkGroupAccess(userId, workspaceId, systemRole))) {
      return c.json({ error: "このグループへのアクセス権がありません" }, 403);
    }

    const summary = await chatSummaryRepo.findById(summaryId);
    if (!summary || summary.monitorId !== monitorId || summary.workspaceId !== workspaceId) {
      return c.json({ error: "要約が見つかりません" }, 404);
    }

    await chatSummaryRepo.deleteById(summaryId);
    return c.json({ deleted: summaryId });
  }
);

// ─── Webhook Receivers ────────────────────────────────────────

// POST /webhook/slack — Slack Event APIの受信
machinaRoutes.post("/webhook/slack", async (c) => {
  const body = await c.req.json<Record<string, unknown>>();

  // Slack URL Verification challenge
  if (body.type === "url_verification") {
    return c.json({ challenge: body.challenge });
  }

  // Event callback
  if (body.type === "event_callback") {
    const event = body.event as Record<string, unknown>;
    if (event.type === "message" && !event.subtype) {
      const workspaceId = c.req.query("workspaceId");
      if (!workspaceId) {
        return c.json({ error: "workspaceId query parameter required" }, 400);
      }

      // 非同期処理（レスポンスを先に返す）
      handleSlackMessage(
        event as unknown as Parameters<typeof handleSlackMessage>[0],
        workspaceId
      ).catch((err: unknown) => {
        console.error("[machina:webhook:slack] メッセージ処理エラー:", err);
      });
    }
  }

  return c.json({ ok: true });
});

// POST /webhook/discord — Discord Webhook の受信
machinaRoutes.post("/webhook/discord", async (c) => {
  const body = await c.req.json<Record<string, unknown>>();
  const workspaceId = c.req.query("workspaceId");

  if (!workspaceId) {
    return c.json({ error: "workspaceId query parameter required" }, 400);
  }

  // Discord のメッセージイベント
  if (body.content && body.author) {
    handleDiscordMessage(
      body as unknown as Parameters<typeof handleDiscordMessage>[0],
      workspaceId
    ).catch((err: unknown) => {
      console.error("[machina:webhook:discord] メッセージ処理エラー:", err);
    });
  }

  return c.json({ ok: true });
});

// ─── Text Analysis (Preview) ─────────────────────────────────

// POST /analyze — テキスト解析プレビュー
machinaRoutes.post("/analyze", async (c) => {
  const userId = getUserId(c);
  if (!userId) return c.json({ error: "Authentication required" }, 401);

  const body = await c.req.json<{
    text: string;
    platform?: string;
  }>();

  if (!body.text) {
    return c.json({ error: "text は必須です" }, 400);
  }

  const result = analyzeMessage({
    text: body.text,
    platform: (body.platform as "slack" | "discord") || "slack",
  });

  return c.json({ analysis: result });
});

// ─── Status / Info ────────────────────────────────────────────

// GET /status — MACHINA モジュールの状態
machinaRoutes.get("/status", async (c) => {
  const userId = getUserId(c);
  if (!userId) return c.json({ error: "Authentication required" }, 401);

  return c.json({
    module: "M3 MACHINA",
    description: "タスク自動生成モジュール",
    pmRelayConnected: hasPmRelay(),
    features: [
      "Slack/Discord チャンネル監視",
      "ルールベースタスク自動生成",
      "自動アサイン / 優先度判定 / 納期設定",
      "PM (M2) リレー",
      "チャンネルモード (task/discussion)",
    ],
  });
});

// ─── Channel Mode Sessions (オンメモリの処理状況) ────────────

// GET /groups/:workspaceId/mode-sessions — 進行中セッションの一覧
machinaRoutes.get("/groups/:workspaceId/mode-sessions", async (c) => {
  const userId = getUserId(c);
  if (!userId) return c.json({ error: "Authentication required" }, 401);
  const systemRole = getUserRole(c);
  const workspaceId = c.req.param("workspaceId");

  if (!(await checkGroupAccess(userId, workspaceId, systemRole))) {
    return c.json({ error: "このグループへのアクセス権がありません" }, 403);
  }

  const taskSessions = taskSessionStore.listByWorkspace(workspaceId).map(serializeTaskSession);
  const discussionSessions = discussionSessionStore
    .listByWorkspace(workspaceId)
    .map(serializeDiscussionSession);

  // 5 分以上 hearing のまま動いていないセッションを「停滞」として印をつける
  const now = Date.now();
  const STALL_THRESHOLD_MS = 5 * 60_000;
  const taskWithFlag = taskSessions.map((s) => ({
    ...s,
    isStalled:
      (s.status === "hearing" || s.status === "failed") &&
      now - new Date(s.updatedAt).getTime() > STALL_THRESHOLD_MS,
  }));
  const discWithFlag = discussionSessions.map((s) => ({
    ...s,
    isStalled: s.status === "failed" || now - new Date(s.scheduledAt).getTime() > STALL_THRESHOLD_MS,
  }));

  return c.json({
    taskSessions: taskWithFlag,
    discussionSessions: discWithFlag,
  });
});

// POST /groups/:workspaceId/mode-sessions/task/:sessionId/resume
// 手動でヒアリング回答 (補足) を注入して再分類する
machinaRoutes.post(
  "/groups/:workspaceId/mode-sessions/task/:sessionId/resume",
  async (c) => {
    const userId = getUserId(c);
    if (!userId) return c.json({ error: "Authentication required" }, 401);
    const systemRole = getUserRole(c);
    const workspaceId = c.req.param("workspaceId");
    const sessionId = c.req.param("sessionId");

    if (!(await checkGroupAccess(userId, workspaceId, systemRole))) {
      return c.json({ error: "このグループへのアクセス権がありません" }, 403);
    }

    const session = taskSessionStore.findById(sessionId);
    if (!session || session.workspaceId !== workspaceId) {
      return c.json({ error: "セッションが見つかりません" }, 404);
    }

    const body = await c.req
      .json<{ supplement: string }>()
      .catch(() => ({ supplement: "" } as { supplement: string }));
    if (!body.supplement || !body.supplement.trim()) {
      return c.json({ error: "supplement は必須です" }, 400);
    }

    const result = await resumeSession(sessionId, body.supplement.trim(), userId);
    return c.json(result);
  }
);

// DELETE /groups/:workspaceId/mode-sessions/task/:sessionId — 破棄
machinaRoutes.delete(
  "/groups/:workspaceId/mode-sessions/task/:sessionId",
  async (c) => {
    const userId = getUserId(c);
    if (!userId) return c.json({ error: "Authentication required" }, 401);
    const systemRole = getUserRole(c);
    const workspaceId = c.req.param("workspaceId");
    const sessionId = c.req.param("sessionId");

    if (!(await checkGroupAccess(userId, workspaceId, systemRole))) {
      return c.json({ error: "このグループへのアクセス権がありません" }, 403);
    }

    const session = taskSessionStore.findById(sessionId);
    if (!session || session.workspaceId !== workspaceId) {
      return c.json({ error: "セッションが見つかりません" }, 404);
    }

    dismissSession(sessionId);
    return c.json({ dismissed: sessionId });
  }
);

// POST /groups/:workspaceId/mode-sessions/discussion/:sessionId/flush
// タイマーを待たずに即時実行する
machinaRoutes.post(
  "/groups/:workspaceId/mode-sessions/discussion/:sessionId/flush",
  async (c) => {
    const userId = getUserId(c);
    if (!userId) return c.json({ error: "Authentication required" }, 401);
    const systemRole = getUserRole(c);
    const workspaceId = c.req.param("workspaceId");
    const sessionId = c.req.param("sessionId");

    if (!(await checkGroupAccess(userId, workspaceId, systemRole))) {
      return c.json({ error: "このグループへのアクセス権がありません" }, 403);
    }

    const session = discussionSessionStore.findById(sessionId);
    if (!session || session.workspaceId !== workspaceId) {
      return c.json({ error: "セッションが見つかりません" }, 404);
    }

    const result = await flushDiscussionSession(sessionId);
    return c.json({ result });
  }
);

// DELETE /groups/:workspaceId/mode-sessions/discussion/:sessionId
machinaRoutes.delete(
  "/groups/:workspaceId/mode-sessions/discussion/:sessionId",
  async (c) => {
    const userId = getUserId(c);
    if (!userId) return c.json({ error: "Authentication required" }, 401);
    const systemRole = getUserRole(c);
    const workspaceId = c.req.param("workspaceId");
    const sessionId = c.req.param("sessionId");

    if (!(await checkGroupAccess(userId, workspaceId, systemRole))) {
      return c.json({ error: "このグループへのアクセス権がありません" }, 403);
    }

    const session = discussionSessionStore.findById(sessionId);
    if (!session || session.workspaceId !== workspaceId) {
      return c.json({ error: "セッションが見つかりません" }, 404);
    }

    dismissDiscussionSession(sessionId);
    return c.json({ dismissed: sessionId });
  }
);
