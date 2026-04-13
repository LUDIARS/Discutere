/**
 * チャンネルモード(task/discussion)の処理状況を管理するオンメモリストア。
 *
 * - タスクモード: 情報不足等でヒアリング中のセッションを追跡
 * - 議論モード : 遅延処理の発火待ちタイマーを追跡
 *
 * どちらも DB を使わずメモリ上で保持する。プロセス再起動で消える。
 * フロントエンドが「処理中/停滞」を可視化し、対応指示を出すために
 * 一覧・詳細・手動進行の API をここに集約する。
 */
import { randomUUID } from "crypto";

// ─── Task mode session ────────────────────────────────

/** タスクモードのセッション状態 */
export type TaskSessionStatus =
  | "classifying" // Haiku に判定問合せ中
  | "hearing" // 情報不足でヒアリング中 (返信待ち)
  | "collecting" // タスク実行開始後、追加投稿を取り込み中
  | "registering" // タスク登録中
  | "failed"; // 処理途中で停止

export interface TaskModeMessage {
  authorId: string;
  authorName: string;
  text: string;
  messageId: string;
  postedAt: Date;
}

export interface TaskSession {
  id: string;
  monitorId: string;
  workspaceId: string;
  platform: "slack" | "discord";
  channelId: string;
  /** Slack: thread_ts、Discord: messageId。同スレッドの追従判定に使用 */
  threadKey: string;
  status: TaskSessionStatus;
  messages: TaskModeMessage[];
  /** Haiku 等の分類結果 */
  classification: {
    isTask: boolean;
    confidence: number;
    missingFields: string[];
    title?: string;
    description?: string;
    priority?: string;
    reasoning: string;
  } | null;
  /** 最終更新 */
  updatedAt: Date;
  createdAt: Date;
  /** 停止理由 (status=failed のとき) */
  errorReason?: string;
  /** 登録済みタスクID (status=registering/collecting 中に作られたタスク) */
  taskId?: string;
}

// ─── Discussion mode timer ────────────────────────────

export type DiscussionStatus =
  | "pending" // 遅延タイマー実行待ち
  | "summarizing" // 要約生成中
  | "publishing" // GitHub Discussion へ保存中
  | "failed";

export interface DiscussionSession {
  id: string;
  monitorId: string;
  workspaceId: string;
  /** タイマー発火予定時刻 */
  scheduledAt: Date;
  /** 対象になる最古のメッセージ時刻 (これ以降を要約対象にする) */
  windowStart: Date;
  status: DiscussionStatus;
  /** 停止理由 */
  errorReason?: string;
  /** 最後に GitHub Discussion に保存した URL */
  lastPublishedUrl?: string;
  updatedAt: Date;
  createdAt: Date;
  /** 内部タイマーハンドル */
  _timer?: ReturnType<typeof setTimeout>;
}

// ─── In-memory stores ─────────────────────────────────

const taskSessions = new Map<string, TaskSession>();
const discussionSessions = new Map<string, DiscussionSession>();

function sessionKey(monitorId: string, threadKey: string): string {
  return `${monitorId}::${threadKey}`;
}

// ─── Task session API ─────────────────────────────────

export const taskSessionStore = {
  /** スレッドに紐づく現行セッションを取得 */
  findActiveByThread(monitorId: string, threadKey: string): TaskSession | undefined {
    const key = sessionKey(monitorId, threadKey);
    const s = taskSessions.get(key);
    if (!s) return undefined;
    // 最終状態 (failed/registering 完了後削除) のものは除外
    return s;
  },

  /** 新規セッションを作成 */
  create(data: Omit<TaskSession, "id" | "createdAt" | "updatedAt">): TaskSession {
    const now = new Date();
    const session: TaskSession = {
      ...data,
      id: randomUUID(),
      createdAt: now,
      updatedAt: now,
    };
    taskSessions.set(sessionKey(session.monitorId, session.threadKey), session);
    return session;
  },

  /** 状態更新 */
  update(id: string, patch: Partial<TaskSession>): TaskSession | undefined {
    for (const [key, s] of taskSessions) {
      if (s.id !== id) continue;
      const next = { ...s, ...patch, updatedAt: new Date() };
      taskSessions.set(key, next);
      return next;
    }
    return undefined;
  },

  /** 破棄 */
  remove(id: string): boolean {
    for (const [key, s] of taskSessions) {
      if (s.id === id) {
        taskSessions.delete(key);
        return true;
      }
    }
    return false;
  },

  findById(id: string): TaskSession | undefined {
    for (const s of taskSessions.values()) if (s.id === id) return s;
    return undefined;
  },

  /** モニタ単位で列挙 */
  listByMonitor(monitorId: string): TaskSession[] {
    const out: TaskSession[] = [];
    for (const s of taskSessions.values()) {
      if (s.monitorId === monitorId) out.push(s);
    }
    return out.sort((a, b) => b.updatedAt.getTime() - a.updatedAt.getTime());
  },

  /** ワークスペース単位で列挙 */
  listByWorkspace(workspaceId: string): TaskSession[] {
    const out: TaskSession[] = [];
    for (const s of taskSessions.values()) {
      if (s.workspaceId === workspaceId) out.push(s);
    }
    return out.sort((a, b) => b.updatedAt.getTime() - a.updatedAt.getTime());
  },

  /** デバッグ/テスト用: 全件クリア */
  clearAll(): void {
    taskSessions.clear();
  },
};

// ─── Discussion session API ───────────────────────────

export const discussionSessionStore = {
  findByMonitor(monitorId: string): DiscussionSession | undefined {
    for (const s of discussionSessions.values()) {
      if (s.monitorId === monitorId) return s;
    }
    return undefined;
  },

  create(data: Omit<DiscussionSession, "id" | "createdAt" | "updatedAt">): DiscussionSession {
    const now = new Date();
    const session: DiscussionSession = {
      ...data,
      id: randomUUID(),
      createdAt: now,
      updatedAt: now,
    };
    discussionSessions.set(session.id, session);
    return session;
  },

  update(id: string, patch: Partial<DiscussionSession>): DiscussionSession | undefined {
    const s = discussionSessions.get(id);
    if (!s) return undefined;
    const next = { ...s, ...patch, updatedAt: new Date() };
    discussionSessions.set(id, next);
    return next;
  },

  remove(id: string): boolean {
    const s = discussionSessions.get(id);
    if (!s) return false;
    if (s._timer) clearTimeout(s._timer);
    return discussionSessions.delete(id);
  },

  findById(id: string): DiscussionSession | undefined {
    return discussionSessions.get(id);
  },

  listByMonitor(monitorId: string): DiscussionSession[] {
    const out: DiscussionSession[] = [];
    for (const s of discussionSessions.values()) {
      if (s.monitorId === monitorId) out.push(s);
    }
    return out.sort((a, b) => b.scheduledAt.getTime() - a.scheduledAt.getTime());
  },

  listByWorkspace(workspaceId: string): DiscussionSession[] {
    const out: DiscussionSession[] = [];
    for (const s of discussionSessions.values()) {
      if (s.workspaceId === workspaceId) out.push(s);
    }
    return out.sort((a, b) => b.scheduledAt.getTime() - a.scheduledAt.getTime());
  },

  clearAll(): void {
    for (const s of discussionSessions.values()) {
      if (s._timer) clearTimeout(s._timer);
    }
    discussionSessions.clear();
  },
};

/** フロントエンド向けに timer 参照などの非シリアライズフィールドを落とす */
export function serializeTaskSession(s: TaskSession) {
  return {
    id: s.id,
    monitorId: s.monitorId,
    workspaceId: s.workspaceId,
    platform: s.platform,
    channelId: s.channelId,
    threadKey: s.threadKey,
    status: s.status,
    messages: s.messages.map((m) => ({
      authorId: m.authorId,
      authorName: m.authorName,
      text: m.text,
      messageId: m.messageId,
      postedAt: m.postedAt.toISOString(),
    })),
    classification: s.classification,
    errorReason: s.errorReason ?? null,
    taskId: s.taskId ?? null,
    createdAt: s.createdAt.toISOString(),
    updatedAt: s.updatedAt.toISOString(),
  };
}

export function serializeDiscussionSession(s: DiscussionSession) {
  return {
    id: s.id,
    monitorId: s.monitorId,
    workspaceId: s.workspaceId,
    scheduledAt: s.scheduledAt.toISOString(),
    windowStart: s.windowStart.toISOString(),
    status: s.status,
    errorReason: s.errorReason ?? null,
    lastPublishedUrl: s.lastPublishedUrl ?? null,
    createdAt: s.createdAt.toISOString(),
    updatedAt: s.updatedAt.toISOString(),
  };
}
