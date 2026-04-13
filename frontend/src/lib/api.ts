import type {
  MachinaTaskItem,
  MachinaTaskLogItem,
  MachinaChannelMonitorItem,
  MachinaChatMessageItem,
  MachinaChatSummaryItem,
  MachinaSummaryHighlights,
  ModeTaskSessionItem,
  ModeDiscussionSessionItem,
  ChannelMode,
  GroupItem,
} from "./api-types";

const STORAGE_KEY = "discutere.user";
const WORKSPACE_KEY = "discutere.workspaces";

interface StoredUser {
  id: string;
  displayName: string;
  role: string;
}

function currentUser(): StoredUser | null {
  try {
    const raw = localStorage.getItem(STORAGE_KEY);
    return raw ? (JSON.parse(raw) as StoredUser) : null;
  } catch {
    return null;
  }
}

async function request<T>(
  method: string,
  path: string,
  body?: unknown
): Promise<T> {
  const user = currentUser();
  const headers: Record<string, string> = {
    "Content-Type": "application/json",
  };
  if (user) {
    headers["X-User-Id"] = user.id;
    headers["X-User-Role"] = user.role;
  }

  const res = await fetch(path, {
    method,
    headers,
    credentials: "include",
    body: body !== undefined ? JSON.stringify(body) : undefined,
  });

  const contentType = res.headers.get("content-type") ?? "";
  const data: unknown = contentType.includes("application/json")
    ? await res.json().catch(() => null)
    : null;

  if (!res.ok) {
    const err = (data as { error?: string } | null)?.error ?? `HTTP ${res.status}`;
    throw new Error(err);
  }
  return data as T;
}

// ─── Machina API ──────────────────────────────────────────

export const machinaApi = {
  // Tasks
  getTasks(workspaceId: string, status?: string) {
    const qs = status ? `?status=${encodeURIComponent(status)}` : "";
    return request<{ tasks: MachinaTaskItem[] }>(
      "GET",
      `/api/groups/${encodeURIComponent(workspaceId)}/tasks${qs}`
    );
  },
  getTask(workspaceId: string, taskId: string) {
    return request<{ task: MachinaTaskItem; logs: MachinaTaskLogItem[] }>(
      "GET",
      `/api/groups/${encodeURIComponent(workspaceId)}/tasks/${encodeURIComponent(taskId)}`
    );
  },
  createTask(
    workspaceId: string,
    data: {
      title: string;
      description?: string;
      priority?: string;
      dueDate?: string;
      assigneeId?: string;
    }
  ) {
    return request<{ id: string; message: string }>(
      "POST",
      `/api/groups/${encodeURIComponent(workspaceId)}/tasks`,
      data
    );
  },
  updateTask(
    workspaceId: string,
    taskId: string,
    data: Partial<{
      title: string;
      description: string;
      status: string;
      priority: string;
      assigneeId: string | null;
      dueDate: string | null;
      isCriticalPath: boolean;
    }>
  ) {
    return request<{ message: string }>(
      "PUT",
      `/api/groups/${encodeURIComponent(workspaceId)}/tasks/${encodeURIComponent(taskId)}`,
      data
    );
  },
  deleteTask(workspaceId: string, taskId: string) {
    return request<{ deleted: string }>(
      "DELETE",
      `/api/groups/${encodeURIComponent(workspaceId)}/tasks/${encodeURIComponent(taskId)}`
    );
  },

  // Monitors
  getMonitors(workspaceId: string) {
    return request<{ monitors: MachinaChannelMonitorItem[] }>(
      "GET",
      `/api/groups/${encodeURIComponent(workspaceId)}/monitors`
    );
  },
  createMonitor(
    workspaceId: string,
    data: {
      platform: string;
      channelId: string;
      channelName: string;
      botToken?: string;
      botWorkspaceId?: string;
      botSigningSecret?: string;
      captureMessages?: boolean;
      mode?: ChannelMode;
      discussionDelayMinutes?: number;
      githubRepo?: string;
      githubDiscussionCategoryId?: string;
    }
  ) {
    return request<{ id: string; message: string }>(
      "POST",
      `/api/groups/${encodeURIComponent(workspaceId)}/monitors`,
      data
    );
  },
  updateMonitor(
    workspaceId: string,
    monitorId: string,
    data: Partial<{
      channelName: string;
      isActive: boolean;
      botToken: string | null;
      botWorkspaceId: string | null;
      botSigningSecret: string | null;
      captureMessages: boolean;
      mode: ChannelMode;
      discussionDelayMinutes: number;
      githubRepo: string | null;
      githubDiscussionCategoryId: string | null;
    }>
  ) {
    return request<{ message: string }>(
      "PUT",
      `/api/groups/${encodeURIComponent(workspaceId)}/monitors/${encodeURIComponent(monitorId)}`,
      data
    );
  },
  deleteMonitor(workspaceId: string, monitorId: string) {
    return request<{ deleted: string }>(
      "DELETE",
      `/api/groups/${encodeURIComponent(workspaceId)}/monitors/${encodeURIComponent(monitorId)}`
    );
  },

  // Chat messages (logs)
  getMessages(workspaceId: string, monitorId: string, limit = 100) {
    return request<{ messages: MachinaChatMessageItem[]; total: number }>(
      "GET",
      `/api/groups/${encodeURIComponent(workspaceId)}/monitors/${encodeURIComponent(monitorId)}/messages?limit=${limit}`
    );
  },

  // Summaries
  getSummaries(workspaceId: string, monitorId: string) {
    return request<{ summaries: MachinaChatSummaryItem[] }>(
      "GET",
      `/api/groups/${encodeURIComponent(workspaceId)}/monitors/${encodeURIComponent(monitorId)}/summaries`
    );
  },
  createSummary(
    workspaceId: string,
    monitorId: string,
    data: { periodStart?: string; periodEnd?: string; hours?: number }
  ) {
    return request<{
      id: string;
      summary: string;
      highlights: MachinaSummaryHighlights;
      messageCount: number;
      periodStart: string;
      periodEnd: string;
    }>(
      "POST",
      `/api/groups/${encodeURIComponent(workspaceId)}/monitors/${encodeURIComponent(monitorId)}/summaries`,
      data
    );
  },
  deleteSummary(workspaceId: string, monitorId: string, summaryId: string) {
    return request<{ deleted: string }>(
      "DELETE",
      `/api/groups/${encodeURIComponent(workspaceId)}/monitors/${encodeURIComponent(monitorId)}/summaries/${encodeURIComponent(summaryId)}`
    );
  },

  // Channel mode sessions (in-memory, 処理状況の可視化)
  getModeSessions(workspaceId: string) {
    return request<{
      taskSessions: ModeTaskSessionItem[];
      discussionSessions: ModeDiscussionSessionItem[];
    }>(
      "GET",
      `/api/groups/${encodeURIComponent(workspaceId)}/mode-sessions`
    );
  },
  resumeTaskSession(workspaceId: string, sessionId: string, supplement: string) {
    return request<{ action: "registered" | "still_hearing" | "not_found" }>(
      "POST",
      `/api/groups/${encodeURIComponent(workspaceId)}/mode-sessions/task/${encodeURIComponent(sessionId)}/resume`,
      { supplement }
    );
  },
  dismissTaskSession(workspaceId: string, sessionId: string) {
    return request<{ dismissed: string }>(
      "DELETE",
      `/api/groups/${encodeURIComponent(workspaceId)}/mode-sessions/task/${encodeURIComponent(sessionId)}`
    );
  },
  flushDiscussionSession(workspaceId: string, sessionId: string) {
    return request<{
      result: {
        messageCount: number;
        summaryId?: string;
        githubUrl?: string;
      } | null;
    }>(
      "POST",
      `/api/groups/${encodeURIComponent(workspaceId)}/mode-sessions/discussion/${encodeURIComponent(sessionId)}/flush`
    );
  },
  dismissDiscussionSession(workspaceId: string, sessionId: string) {
    return request<{ dismissed: string }>(
      "DELETE",
      `/api/groups/${encodeURIComponent(workspaceId)}/mode-sessions/discussion/${encodeURIComponent(sessionId)}`
    );
  },

  // Analyze preview
  analyzeText(text: string, platform: "slack" | "discord" = "slack") {
    return request<{
      analysis: {
        shouldCreateTask: boolean;
        shouldUpdateExisting: boolean;
        title: string;
        description: string | null;
        priority: string;
        assigneeHint: string | null;
        dueDateHint: string | null;
        confidence: number;
        reasoning: string;
        isCompletion: boolean;
      };
    }>("POST", `/api/analyze`, { text, platform });
  },
};

// ─── Group API ────────────────────────────────────────────
// Discutere 単体では Cernere からグループを取得できないため、
// localStorage に保存されたローカルワークスペース一覧を返す。
// 本番では Cernere API 呼び出しに差し替え予定。

function readLocalWorkspaces(): GroupItem[] {
  try {
    const raw = localStorage.getItem(WORKSPACE_KEY);
    if (raw) {
      const arr = JSON.parse(raw) as GroupItem[];
      if (Array.isArray(arr) && arr.length > 0) return arr;
    }
  } catch {
    /* ignore */
  }
  // デフォルト: "default" ワークスペース
  const defaults: GroupItem[] = [{ id: "default", name: "Default Workspace" }];
  localStorage.setItem(WORKSPACE_KEY, JSON.stringify(defaults));
  return defaults;
}

function writeLocalWorkspaces(items: GroupItem[]): void {
  localStorage.setItem(WORKSPACE_KEY, JSON.stringify(items));
}

export const groupApi = {
  async listMyGroups(): Promise<{ groups: GroupItem[] }> {
    return { groups: readLocalWorkspaces() };
  },
  async createGroup(name: string): Promise<GroupItem> {
    const id = (name.trim() || "workspace")
      .toLowerCase()
      .replace(/[^a-z0-9-]+/g, "-")
      .replace(/^-+|-+$/g, "") || `ws-${Date.now()}`;
    const items = readLocalWorkspaces();
    if (items.some((g) => g.id === id)) {
      throw new Error("同じIDのワークスペースが既に存在します");
    }
    const next: GroupItem = { id, name: name.trim() || id };
    writeLocalWorkspaces([...items, next]);
    return next;
  },
};
