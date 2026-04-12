import { useState, useEffect, useCallback } from "react";
import { useAuth } from "../contexts/AuthContext";
import { machinaApi, groupApi } from "../lib/api";
import type {
  MachinaTaskItem,
  MachinaChannelMonitorItem,
  MachinaTaskLogItem,
  MachinaChatMessageItem,
  MachinaChatSummaryItem,
  MachinaSummaryHighlights,
} from "../lib/api-types";

// ─── Constants ────────────────────────────────────────────────

const PRIORITY_LABELS: Record<string, string> = {
  low: "低",
  medium: "中",
  high: "高",
  critical: "緊急",
};

const PRIORITY_COLORS: Record<string, string> = {
  low: "#8B949E",
  medium: "#D29922",
  high: "#F0883E",
  critical: "#F85149",
};

const STATUS_LABELS: Record<string, string> = {
  pending: "未着手",
  in_progress: "進行中",
  done: "完了",
  cancelled: "キャンセル",
};

const STATUS_COLORS: Record<string, string> = {
  pending: "#8B949E",
  in_progress: "#58A6FF",
  done: "#3FB950",
  cancelled: "#484F58",
};

const SOURCE_LABELS: Record<string, string> = {
  auto: "自動検出",
  command: "コマンド",
  manual: "手動",
};

interface GroupOption {
  id: string;
  name: string;
}

// ─── Component ────────────────────────────────────────────────

export function MachinaPage() {
  const { user } = useAuth();
  const [groups, setGroups] = useState<GroupOption[]>([]);
  const [selectedGroupId, setSelectedGroupId] = useState<string>("");
  const [tasks, setTasks] = useState<MachinaTaskItem[]>([]);
  const [monitors, setMonitors] = useState<MachinaChannelMonitorItem[]>([]);
  const [selectedTask, setSelectedTask] = useState<MachinaTaskItem | null>(null);
  const [taskLogs, setTaskLogs] = useState<MachinaTaskLogItem[]>([]);
  const [statusFilter, setStatusFilter] = useState<string>("");
  const [loading, setLoading] = useState(false);
  const [message, setMessage] = useState("");
  const [tab, setTab] = useState<"tasks" | "monitors" | "logs" | "analyze">("tasks");

  // Create task form
  const [showCreateTask, setShowCreateTask] = useState(false);
  const [taskForm, setTaskForm] = useState({
    title: "",
    description: "",
    priority: "medium",
    dueDate: "",
  });

  // Create monitor form
  const [showCreateMonitor, setShowCreateMonitor] = useState(false);
  const [monitorForm, setMonitorForm] = useState({
    platform: "slack",
    channelId: "",
    channelName: "",
    botToken: "",
    botWorkspaceId: "",
    botSigningSecret: "",
    captureMessages: true,
  });

  // Edit bot config for an existing monitor
  const [editingBotMonitorId, setEditingBotMonitorId] = useState<string | null>(null);
  const [botForm, setBotForm] = useState({
    botToken: "",
    botWorkspaceId: "",
    botSigningSecret: "",
    captureMessages: true,
  });

  // Chat logs / summaries
  const [logMonitorId, setLogMonitorId] = useState<string>("");
  const [chatMessages, setChatMessages] = useState<MachinaChatMessageItem[]>([]);
  const [chatTotal, setChatTotal] = useState<number>(0);
  const [summaries, setSummaries] = useState<MachinaChatSummaryItem[]>([]);
  const [summaryHours, setSummaryHours] = useState<number>(24);
  const [summaryLoading, setSummaryLoading] = useState(false);
  const [logsLoading, setLogsLoading] = useState(false);

  // Analyze form
  const [analyzeText, setAnalyzeText] = useState("");
  const [analyzeResult, setAnalyzeResult] = useState<{
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
  } | null>(null);

  const showMsg = (msg: string) => {
    setMessage(msg);
    setTimeout(() => setMessage(""), 4000);
  };

  // ─── Fetch Groups ───────────────────────────────────────────

  const fetchGroups = useCallback(async () => {
    try {
      const res = await groupApi.listMyGroups();
      const groupList = (res.groups || []).map((g: { id: string; name: string }) => ({
        id: g.id,
        name: g.name,
      }));
      setGroups(groupList);
      if (groupList.length > 0 && !selectedGroupId) {
        setSelectedGroupId(groupList[0].id);
      }
    } catch (e) {
      console.error("[MachinaPage] fetchGroups:", e);
    }
  }, [selectedGroupId]);

  useEffect(() => {
    fetchGroups();
  }, [fetchGroups]);

  // ─── Fetch Tasks ────────────────────────────────────────────

  const fetchTasks = useCallback(async () => {
    if (!selectedGroupId) return;
    setLoading(true);
    try {
      const res = await machinaApi.getTasks(selectedGroupId, statusFilter || undefined);
      setTasks(res.tasks || []);
    } catch (e) {
      const err = e instanceof Error ? e.message : String(e);
      console.error("[MachinaPage] fetchTasks:", err);
      showMsg(`エラー: ${err}`);
    } finally {
      setLoading(false);
    }
  }, [selectedGroupId, statusFilter]);

  useEffect(() => {
    if (tab === "tasks") fetchTasks();
  }, [fetchTasks, tab]);

  // ─── Fetch Monitors ─────────────────────────────────────────

  const fetchMonitors = useCallback(async () => {
    if (!selectedGroupId) return;
    try {
      const res = await machinaApi.getMonitors(selectedGroupId);
      setMonitors(res.monitors || []);
    } catch (e) {
      const err = e instanceof Error ? e.message : String(e);
      console.error("[MachinaPage] fetchMonitors:", err);
    }
  }, [selectedGroupId]);

  useEffect(() => {
    if (tab === "monitors") fetchMonitors();
  }, [fetchMonitors, tab]);

  // ─── Task Detail ────────────────────────────────────────────

  const fetchTaskDetail = async (taskId: string) => {
    if (!selectedGroupId) return;
    try {
      const res = await machinaApi.getTask(selectedGroupId, taskId);
      setSelectedTask(res.task);
      setTaskLogs(res.logs || []);
    } catch (e) {
      const err = e instanceof Error ? e.message : String(e);
      showMsg(`エラー: ${err}`);
    }
  };

  // ─── Create Task ────────────────────────────────────────────

  const handleCreateTask = async () => {
    if (!selectedGroupId || !taskForm.title.trim()) return;
    try {
      await machinaApi.createTask(selectedGroupId, {
        title: taskForm.title,
        description: taskForm.description || undefined,
        priority: taskForm.priority,
        dueDate: taskForm.dueDate || undefined,
      });
      showMsg("タスクを作成しました");
      setShowCreateTask(false);
      setTaskForm({ title: "", description: "", priority: "medium", dueDate: "" });
      fetchTasks();
    } catch (e) {
      const err = e instanceof Error ? e.message : String(e);
      showMsg(`エラー: ${err}`);
    }
  };

  // ─── Update Task Status ─────────────────────────────────────

  const handleUpdateStatus = async (taskId: string, status: string) => {
    if (!selectedGroupId) return;
    try {
      await machinaApi.updateTask(selectedGroupId, taskId, { status });
      showMsg(`ステータスを「${STATUS_LABELS[status] || status}」に変更しました`);
      fetchTasks();
      if (selectedTask?.id === taskId) {
        fetchTaskDetail(taskId);
      }
    } catch (e) {
      const err = e instanceof Error ? e.message : String(e);
      showMsg(`エラー: ${err}`);
    }
  };

  // ─── Delete Task ────────────────────────────────────────────

  const handleDeleteTask = async (taskId: string) => {
    if (!selectedGroupId) return;
    if (!confirm("このタスクを削除しますか？")) return;
    try {
      await machinaApi.deleteTask(selectedGroupId, taskId);
      showMsg("タスクを削除しました");
      if (selectedTask?.id === taskId) setSelectedTask(null);
      fetchTasks();
    } catch (e) {
      const err = e instanceof Error ? e.message : String(e);
      showMsg(`エラー: ${err}`);
    }
  };

  // ─── Create Monitor ─────────────────────────────────────────

  const handleCreateMonitor = async () => {
    if (!selectedGroupId || !monitorForm.channelId.trim() || !monitorForm.channelName.trim()) return;
    try {
      await machinaApi.createMonitor(selectedGroupId, {
        platform: monitorForm.platform,
        channelId: monitorForm.channelId,
        channelName: monitorForm.channelName,
        botToken: monitorForm.botToken.trim() || undefined,
        botWorkspaceId: monitorForm.botWorkspaceId.trim() || undefined,
        botSigningSecret: monitorForm.botSigningSecret.trim() || undefined,
        captureMessages: monitorForm.captureMessages,
      });
      showMsg("チャンネル監視を追加しました");
      setShowCreateMonitor(false);
      setMonitorForm({
        platform: "slack",
        channelId: "",
        channelName: "",
        botToken: "",
        botWorkspaceId: "",
        botSigningSecret: "",
        captureMessages: true,
      });
      fetchMonitors();
    } catch (e) {
      const err = e instanceof Error ? e.message : String(e);
      showMsg(`エラー: ${err}`);
    }
  };

  // ─── Edit Bot Config ────────────────────────────────────────

  const openBotEdit = (m: MachinaChannelMonitorItem) => {
    setEditingBotMonitorId(m.id);
    setBotForm({
      botToken: "",
      botWorkspaceId: m.botWorkspaceId ?? "",
      botSigningSecret: "",
      captureMessages: m.captureMessages,
    });
  };

  const handleSaveBotConfig = async () => {
    if (!selectedGroupId || !editingBotMonitorId) return;
    try {
      const payload: Record<string, unknown> = {
        captureMessages: botForm.captureMessages,
        botWorkspaceId: botForm.botWorkspaceId.trim() || null,
      };
      // 空欄はトークン維持扱い。明示的にクリアしたい場合は "-" を入力する運用。
      if (botForm.botToken.trim()) {
        payload.botToken = botForm.botToken.trim() === "-" ? null : botForm.botToken.trim();
      }
      if (botForm.botSigningSecret.trim()) {
        payload.botSigningSecret =
          botForm.botSigningSecret.trim() === "-" ? null : botForm.botSigningSecret.trim();
      }
      await machinaApi.updateMonitor(selectedGroupId, editingBotMonitorId, payload);
      showMsg("BOT 設定を更新しました");
      setEditingBotMonitorId(null);
      fetchMonitors();
    } catch (e) {
      const err = e instanceof Error ? e.message : String(e);
      showMsg(`エラー: ${err}`);
    }
  };

  // ─── Toggle Monitor ─────────────────────────────────────────

  const handleToggleMonitor = async (monitorId: string, isActive: boolean) => {
    if (!selectedGroupId) return;
    try {
      await machinaApi.updateMonitor(selectedGroupId, monitorId, { isActive: !isActive });
      fetchMonitors();
    } catch (e) {
      const err = e instanceof Error ? e.message : String(e);
      showMsg(`エラー: ${err}`);
    }
  };

  // ─── Delete Monitor ─────────────────────────────────────────

  const handleDeleteMonitor = async (monitorId: string) => {
    if (!selectedGroupId) return;
    if (!confirm("この監視設定を削除しますか？")) return;
    try {
      await machinaApi.deleteMonitor(selectedGroupId, monitorId);
      showMsg("チャンネル監視を削除しました");
      fetchMonitors();
    } catch (e) {
      const err = e instanceof Error ? e.message : String(e);
      showMsg(`エラー: ${err}`);
    }
  };

  // ─── Chat Logs / Summaries ──────────────────────────────────

  const fetchChatLogs = useCallback(async () => {
    if (!selectedGroupId || !logMonitorId) return;
    setLogsLoading(true);
    try {
      const [msgRes, sumRes] = await Promise.all([
        machinaApi.getMessages(selectedGroupId, logMonitorId, 200),
        machinaApi.getSummaries(selectedGroupId, logMonitorId),
      ]);
      setChatMessages(msgRes.messages || []);
      setChatTotal(msgRes.total || 0);
      setSummaries(sumRes.summaries || []);
    } catch (e) {
      const err = e instanceof Error ? e.message : String(e);
      showMsg(`エラー: ${err}`);
    } finally {
      setLogsLoading(false);
    }
  }, [selectedGroupId, logMonitorId]);

  // Logs タブに切り替えたら監視一覧を取り込む
  useEffect(() => {
    if (tab === "logs") {
      fetchMonitors();
    }
  }, [tab, fetchMonitors]);

  // 監視が取得されたら、ログ対象の初期値を設定
  useEffect(() => {
    if (tab !== "logs") return;
    if (monitors.length > 0 && !logMonitorId) {
      setLogMonitorId(monitors[0].id);
    }
  }, [tab, monitors, logMonitorId]);

  // 対象監視が決まったらログを取得
  useEffect(() => {
    if (tab === "logs" && logMonitorId) {
      fetchChatLogs();
    }
  }, [tab, logMonitorId, fetchChatLogs]);

  const handleCreateSummary = async () => {
    if (!selectedGroupId || !logMonitorId) return;
    setSummaryLoading(true);
    try {
      await machinaApi.createSummary(selectedGroupId, logMonitorId, {
        hours: summaryHours,
      });
      showMsg("要約を生成しました");
      fetchChatLogs();
    } catch (e) {
      const err = e instanceof Error ? e.message : String(e);
      showMsg(`エラー: ${err}`);
    } finally {
      setSummaryLoading(false);
    }
  };

  const handleDeleteSummary = async (summaryId: string) => {
    if (!selectedGroupId || !logMonitorId) return;
    if (!confirm("この要約を削除しますか？")) return;
    try {
      await machinaApi.deleteSummary(selectedGroupId, logMonitorId, summaryId);
      showMsg("要約を削除しました");
      fetchChatLogs();
    } catch (e) {
      const err = e instanceof Error ? e.message : String(e);
      showMsg(`エラー: ${err}`);
    }
  };

  const parseHighlights = (raw: string | null): MachinaSummaryHighlights | null => {
    if (!raw) return null;
    try {
      return JSON.parse(raw) as MachinaSummaryHighlights;
    } catch {
      return null;
    }
  };

  // ─── Analyze ────────────────────────────────────────────────

  const handleAnalyze = async () => {
    if (!analyzeText.trim()) return;
    try {
      const res = await machinaApi.analyzeText(analyzeText);
      setAnalyzeResult(res.analysis);
    } catch (e) {
      const err = e instanceof Error ? e.message : String(e);
      showMsg(`エラー: ${err}`);
    }
  };

  // ─── Render ─────────────────────────────────────────────────

  if (!user) return null;

  return (
    <div style={{ maxWidth: "1100px", margin: "0 auto", padding: "0 1rem" }}>
      <div className="page-header">
        <h2 style={{ fontSize: "1.1rem", fontWeight: 700 }}>M3 MACHINA — タスク自動生成</h2>
      </div>

      {message && (
        <div
          style={{
            padding: "0.5rem 0.75rem",
            marginBottom: "1rem",
            borderRadius: "var(--radius-sm)",
            background: message.startsWith("エラー") ? "rgba(248,81,73,0.15)" : "rgba(63,185,80,0.15)",
            color: message.startsWith("エラー") ? "#F85149" : "#3FB950",
            fontSize: "0.8rem",
          }}
        >
          {message}
        </div>
      )}

      {/* Group Selector */}
      <div style={{ marginBottom: "1rem", display: "flex", gap: "0.75rem", alignItems: "center", flexWrap: "wrap" }}>
        <label style={{ fontSize: "0.8rem", color: "var(--text-muted)" }}>グループ:</label>
        <select
          value={selectedGroupId}
          onChange={(e) => { setSelectedGroupId(e.target.value); setSelectedTask(null); }}
          style={{ fontSize: "0.8rem", padding: "0.3rem 0.5rem" }}
        >
          {groups.map((g) => (
            <option key={g.id} value={g.id}>{g.name}</option>
          ))}
        </select>

        {/* Tabs */}
        <div style={{ display: "flex", gap: "0.25rem", marginLeft: "auto" }}>
          {(["tasks", "monitors", "logs", "analyze"] as const).map((t) => (
            <button
              key={t}
              onClick={() => setTab(t)}
              className={tab === t ? "btn-primary" : "btn-secondary"}
              style={{ fontSize: "0.75rem", padding: "0.25rem 0.6rem" }}
            >
              {t === "tasks"
                ? "タスク"
                : t === "monitors"
                ? "監視設定"
                : t === "logs"
                ? "チャットログ"
                : "テキスト解析"}
            </button>
          ))}
        </div>
      </div>

      {/* ─── Tasks Tab ──────────────────────────────────────── */}
      {tab === "tasks" && (
        <div>
          <div style={{ display: "flex", gap: "0.5rem", marginBottom: "1rem", alignItems: "center", flexWrap: "wrap" }}>
            <select
              value={statusFilter}
              onChange={(e) => setStatusFilter(e.target.value)}
              style={{ fontSize: "0.8rem", padding: "0.3rem 0.5rem" }}
            >
              <option value="">全てのステータス</option>
              <option value="pending">未着手</option>
              <option value="in_progress">進行中</option>
              <option value="done">完了</option>
              <option value="cancelled">キャンセル</option>
            </select>
            <button
              className="btn-primary"
              onClick={() => setShowCreateTask(true)}
              style={{ fontSize: "0.75rem", padding: "0.3rem 0.6rem", marginLeft: "auto" }}
            >
              + タスク作成
            </button>
          </div>

          {/* Create Task Form */}
          {showCreateTask && (
            <div className="card" style={{ marginBottom: "1rem" }}>
              <h3 style={{ fontSize: "0.85rem", marginBottom: "0.75rem", color: "var(--text-muted)" }}>新規タスク作成</h3>
              <div style={{ display: "grid", gridTemplateColumns: "1fr 1fr", gap: "0.75rem" }}>
                <div className="form-group" style={{ gridColumn: "1 / -1" }}>
                  <label>タイトル *</label>
                  <input
                    type="text"
                    value={taskForm.title}
                    onChange={(e) => setTaskForm({ ...taskForm, title: e.target.value })}
                    placeholder="タスクタイトル"
                  />
                </div>
                <div className="form-group" style={{ gridColumn: "1 / -1" }}>
                  <label>説明</label>
                  <textarea
                    value={taskForm.description}
                    onChange={(e) => setTaskForm({ ...taskForm, description: e.target.value })}
                    placeholder="タスクの詳細説明"
                    rows={3}
                  />
                </div>
                <div className="form-group">
                  <label>優先度</label>
                  <select
                    value={taskForm.priority}
                    onChange={(e) => setTaskForm({ ...taskForm, priority: e.target.value })}
                  >
                    <option value="low">低</option>
                    <option value="medium">中</option>
                    <option value="high">高</option>
                    <option value="critical">緊急</option>
                  </select>
                </div>
                <div className="form-group">
                  <label>納期</label>
                  <input
                    type="date"
                    value={taskForm.dueDate}
                    onChange={(e) => setTaskForm({ ...taskForm, dueDate: e.target.value })}
                  />
                </div>
              </div>
              <div style={{ display: "flex", gap: "0.5rem", marginTop: "0.75rem" }}>
                <button className="btn-primary" onClick={handleCreateTask} style={{ fontSize: "0.75rem" }}>作成</button>
                <button className="btn-secondary" onClick={() => setShowCreateTask(false)} style={{ fontSize: "0.75rem" }}>キャンセル</button>
              </div>
            </div>
          )}

          {/* Task List */}
          {loading ? (
            <p style={{ color: "var(--text-muted)", fontSize: "0.8rem" }}>読み込み中...</p>
          ) : tasks.length === 0 ? (
            <p style={{ color: "var(--text-muted)", fontSize: "0.8rem" }}>タスクがありません</p>
          ) : (
            <div style={{ display: "flex", gap: "1rem", alignItems: "flex-start" }}>
              {/* Task List Panel */}
              <div style={{ flex: "1 1 0", minWidth: 0 }}>
                <div style={{ display: "flex", flexDirection: "column", gap: "0.5rem" }}>
                  {tasks.map((task) => (
                    <div
                      key={task.id}
                      className="card"
                      onClick={() => fetchTaskDetail(task.id)}
                      style={{
                        cursor: "pointer",
                        padding: "0.6rem 0.75rem",
                        border: selectedTask?.id === task.id ? "1px solid var(--accent)" : "1px solid var(--border)",
                        transition: "border-color 0.15s",
                      }}
                    >
                      <div style={{ display: "flex", justifyContent: "space-between", alignItems: "flex-start", gap: "0.5rem" }}>
                        <div style={{ flex: 1, minWidth: 0 }}>
                          <div style={{ fontSize: "0.85rem", fontWeight: 600, marginBottom: "0.25rem" }}>
                            {task.title}
                          </div>
                          <div style={{ display: "flex", gap: "0.4rem", flexWrap: "wrap", fontSize: "0.7rem" }}>
                            <span style={{
                              background: STATUS_COLORS[task.status] + "22",
                              color: STATUS_COLORS[task.status],
                              padding: "0.1rem 0.35rem",
                              borderRadius: "3px",
                            }}>
                              {STATUS_LABELS[task.status] || task.status}
                            </span>
                            <span style={{
                              background: PRIORITY_COLORS[task.priority] + "22",
                              color: PRIORITY_COLORS[task.priority],
                              padding: "0.1rem 0.35rem",
                              borderRadius: "3px",
                            }}>
                              {PRIORITY_LABELS[task.priority] || task.priority}
                            </span>
                            <span style={{ color: "var(--text-muted)" }}>
                              {SOURCE_LABELS[task.source] || task.source}
                            </span>
                            {task.assigneeName && (
                              <span style={{ color: "var(--text-muted)" }}>
                                → {task.assigneeName}
                              </span>
                            )}
                            {task.dueDate && (
                              <span style={{ color: "var(--text-muted)" }}>
                                期限: {task.dueDate.slice(0, 10)}
                              </span>
                            )}
                          </div>
                        </div>
                        <div style={{ display: "flex", gap: "0.25rem" }}>
                          {task.status !== "done" && (
                            <button
                              onClick={(e) => { e.stopPropagation(); handleUpdateStatus(task.id, task.status === "pending" ? "in_progress" : "done"); }}
                              className="btn-secondary"
                              style={{ fontSize: "0.65rem", padding: "0.15rem 0.35rem" }}
                            >
                              {task.status === "pending" ? "開始" : "完了"}
                            </button>
                          )}
                          <button
                            onClick={(e) => { e.stopPropagation(); handleDeleteTask(task.id); }}
                            className="btn-secondary"
                            style={{ fontSize: "0.65rem", padding: "0.15rem 0.35rem", color: "#F85149" }}
                          >
                            削除
                          </button>
                        </div>
                      </div>
                    </div>
                  ))}
                </div>
              </div>

              {/* Task Detail Panel */}
              {selectedTask && (
                <div className="card" style={{ flex: "0 0 380px", maxWidth: "380px" }}>
                  <h3 style={{ fontSize: "0.9rem", fontWeight: 700, marginBottom: "0.5rem" }}>
                    {selectedTask.title}
                  </h3>
                  {selectedTask.description && (
                    <p style={{ fontSize: "0.8rem", color: "var(--text-muted)", marginBottom: "0.5rem" }}>
                      {selectedTask.description}
                    </p>
                  )}
                  <div style={{ fontSize: "0.75rem", display: "flex", flexDirection: "column", gap: "0.3rem", marginBottom: "0.75rem" }}>
                    <div><strong>ステータス:</strong> {STATUS_LABELS[selectedTask.status] || selectedTask.status}</div>
                    <div><strong>優先度:</strong> {PRIORITY_LABELS[selectedTask.priority] || selectedTask.priority}</div>
                    <div><strong>担当:</strong> {selectedTask.assigneeName || "未アサイン"}</div>
                    <div><strong>納期:</strong> {selectedTask.dueDate ? selectedTask.dueDate.slice(0, 10) : "未設定"}</div>
                    <div><strong>生成元:</strong> {SOURCE_LABELS[selectedTask.source] || selectedTask.source}
                      {selectedTask.sourcePlatform && ` (${selectedTask.sourcePlatform})`}
                    </div>
                    <div><strong>信頼度:</strong> {selectedTask.confidence}%</div>
                    {selectedTask.isCriticalPath && (
                      <div style={{ color: "#F85149" }}><strong>クリティカルパス</strong></div>
                    )}
                    {selectedTask.relayedToPm && (
                      <div style={{ color: "#3FB950" }}><strong>PM リレー済み</strong> (ID: {selectedTask.pmTaskId})</div>
                    )}
                  </div>

                  {selectedTask.sourceText && (
                    <div style={{ marginBottom: "0.75rem" }}>
                      <strong style={{ fontSize: "0.75rem" }}>元メッセージ:</strong>
                      <div style={{
                        fontSize: "0.7rem",
                        background: "var(--bg-surface-2)",
                        padding: "0.4rem",
                        borderRadius: "4px",
                        marginTop: "0.25rem",
                        whiteSpace: "pre-wrap",
                        maxHeight: "100px",
                        overflow: "auto",
                      }}>
                        {selectedTask.sourceText}
                      </div>
                    </div>
                  )}

                  {/* Status Actions */}
                  <div style={{ display: "flex", gap: "0.3rem", marginBottom: "0.75rem", flexWrap: "wrap" }}>
                    {(["pending", "in_progress", "done", "cancelled"] as const).map((s) => (
                      <button
                        key={s}
                        onClick={() => handleUpdateStatus(selectedTask.id, s)}
                        disabled={selectedTask.status === s}
                        className={selectedTask.status === s ? "btn-primary" : "btn-secondary"}
                        style={{ fontSize: "0.65rem", padding: "0.15rem 0.35rem" }}
                      >
                        {STATUS_LABELS[s]}
                      </button>
                    ))}
                  </div>

                  {/* Task Logs */}
                  {taskLogs.length > 0 && (
                    <div>
                      <h4 style={{ fontSize: "0.75rem", color: "var(--text-muted)", marginBottom: "0.3rem" }}>変更履歴</h4>
                      <div style={{ fontSize: "0.7rem", display: "flex", flexDirection: "column", gap: "0.2rem" }}>
                        {taskLogs.slice(0, 10).map((log) => (
                          <div key={log.id} style={{ color: "var(--text-muted)", borderLeft: "2px solid var(--border)", paddingLeft: "0.4rem" }}>
                            <div><strong>{log.action}</strong> — {log.performedBy === "system" ? "自動" : log.performedBy}</div>
                            {log.reason && <div>{log.reason}</div>}
                            <div style={{ fontSize: "0.65rem" }}>{new Date(log.createdAt).toLocaleString("ja-JP")}</div>
                          </div>
                        ))}
                      </div>
                    </div>
                  )}
                </div>
              )}
            </div>
          )}
        </div>
      )}

      {/* ─── Monitors Tab ───────────────────────────────────── */}
      {tab === "monitors" && (
        <div>
          <div style={{ marginBottom: "1rem" }}>
            <button
              className="btn-primary"
              onClick={() => setShowCreateMonitor(true)}
              style={{ fontSize: "0.75rem", padding: "0.3rem 0.6rem" }}
            >
              + チャンネル監視追加
            </button>
          </div>

          {/* Create Monitor Form */}
          {showCreateMonitor && (
            <div className="card" style={{ marginBottom: "1rem" }}>
              <h3 style={{ fontSize: "0.85rem", marginBottom: "0.75rem", color: "var(--text-muted)" }}>
                BOT チャンネル設定
              </h3>
              <div style={{ display: "grid", gridTemplateColumns: "1fr 1fr 1fr", gap: "0.75rem" }}>
                <div className="form-group">
                  <label>プラットフォーム *</label>
                  <select
                    value={monitorForm.platform}
                    onChange={(e) => setMonitorForm({ ...monitorForm, platform: e.target.value })}
                  >
                    <option value="slack">Slack</option>
                    <option value="discord">Discord</option>
                  </select>
                </div>
                <div className="form-group">
                  <label>チャンネルID *</label>
                  <input
                    type="text"
                    value={monitorForm.channelId}
                    onChange={(e) => setMonitorForm({ ...monitorForm, channelId: e.target.value })}
                    placeholder={monitorForm.platform === "slack" ? "C01234ABCDE" : "123456789012345678"}
                  />
                </div>
                <div className="form-group">
                  <label>チャンネル名 *</label>
                  <input
                    type="text"
                    value={monitorForm.channelName}
                    onChange={(e) => setMonitorForm({ ...monitorForm, channelName: e.target.value })}
                    placeholder="#general"
                  />
                </div>
                <div className="form-group" style={{ gridColumn: "1 / -1" }}>
                  <label>BOT トークン (導入した BOT の認証情報)</label>
                  <input
                    type="password"
                    autoComplete="new-password"
                    value={monitorForm.botToken}
                    onChange={(e) => setMonitorForm({ ...monitorForm, botToken: e.target.value })}
                    placeholder={monitorForm.platform === "slack" ? "xoxb-..." : "Discord Bot Token"}
                  />
                </div>
                <div className="form-group">
                  <label>
                    {monitorForm.platform === "slack" ? "Slack Workspace ID" : "Discord Guild ID"}
                  </label>
                  <input
                    type="text"
                    value={monitorForm.botWorkspaceId}
                    onChange={(e) => setMonitorForm({ ...monitorForm, botWorkspaceId: e.target.value })}
                    placeholder={monitorForm.platform === "slack" ? "T01ABCDEFGH" : "Guild ID"}
                  />
                </div>
                <div className="form-group" style={{ gridColumn: "span 2" }}>
                  <label>
                    {monitorForm.platform === "slack" ? "Signing Secret" : "Application Public Key"}
                  </label>
                  <input
                    type="password"
                    autoComplete="new-password"
                    value={monitorForm.botSigningSecret}
                    onChange={(e) => setMonitorForm({ ...monitorForm, botSigningSecret: e.target.value })}
                    placeholder="(任意) Webhook 署名検証用"
                  />
                </div>
                <div className="form-group" style={{ gridColumn: "1 / -1" }}>
                  <label style={{ display: "flex", alignItems: "center", gap: "0.4rem", cursor: "pointer" }}>
                    <input
                      type="checkbox"
                      checked={monitorForm.captureMessages}
                      onChange={(e) =>
                        setMonitorForm({ ...monitorForm, captureMessages: e.target.checked })
                      }
                      style={{ width: "auto" }}
                    />
                    チャンネルのメッセージを取得してログに保存する
                  </label>
                </div>
              </div>
              <p style={{ fontSize: "0.7rem", color: "var(--text-muted)", marginTop: "0.5rem" }}>
                BOT の Webhook 先: <code>/api/webhook/{monitorForm.platform}?workspaceId={selectedGroupId}</code>
              </p>
              <div style={{ display: "flex", gap: "0.5rem", marginTop: "0.75rem" }}>
                <button className="btn-primary" onClick={handleCreateMonitor} style={{ fontSize: "0.75rem" }}>追加</button>
                <button className="btn-secondary" onClick={() => setShowCreateMonitor(false)} style={{ fontSize: "0.75rem" }}>キャンセル</button>
              </div>
            </div>
          )}

          {/* Monitor List */}
          {monitors.length === 0 ? (
            <p style={{ color: "var(--text-muted)", fontSize: "0.8rem" }}>チャンネル監視が設定されていません</p>
          ) : (
            <div style={{ display: "flex", flexDirection: "column", gap: "0.5rem" }}>
              {monitors.map((m) => (
                <div key={m.id} className="card" style={{ padding: "0.6rem 0.75rem" }}>
                  <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center", gap: "0.5rem", flexWrap: "wrap" }}>
                    <div style={{ flex: 1, minWidth: 0 }}>
                      <span style={{ fontSize: "0.85rem", fontWeight: 600 }}>{m.channelName}</span>
                      <span style={{
                        fontSize: "0.7rem",
                        marginLeft: "0.5rem",
                        background: m.platform === "slack" ? "#4A154B22" : "#5865F222",
                        color: m.platform === "slack" ? "#4A154B" : "#5865F2",
                        padding: "0.1rem 0.35rem",
                        borderRadius: "3px",
                      }}>
                        {m.platform}
                      </span>
                      <span style={{
                        fontSize: "0.7rem",
                        marginLeft: "0.3rem",
                        color: m.isActive ? "#3FB950" : "#8B949E",
                      }}>
                        {m.isActive ? "有効" : "無効"}
                      </span>
                      <span style={{
                        fontSize: "0.7rem",
                        marginLeft: "0.3rem",
                        padding: "0.1rem 0.35rem",
                        borderRadius: "3px",
                        background: m.hasBotToken ? "#3FB95022" : "#8B949E22",
                        color: m.hasBotToken ? "#3FB950" : "#8B949E",
                      }}>
                        {m.hasBotToken ? "BOT接続済" : "BOT未設定"}
                      </span>
                      <span style={{
                        fontSize: "0.7rem",
                        marginLeft: "0.3rem",
                        color: m.captureMessages ? "var(--accent)" : "var(--text-muted)",
                      }}>
                        {m.captureMessages ? "ログ取得ON" : "ログ取得OFF"}
                      </span>
                      <span style={{ fontSize: "0.7rem", color: "var(--text-muted)", marginLeft: "0.5rem" }}>
                        ID: {m.channelId}
                      </span>
                    </div>
                    <div style={{ display: "flex", gap: "0.3rem" }}>
                      <button
                        className="btn-secondary"
                        onClick={() => openBotEdit(m)}
                        style={{ fontSize: "0.65rem", padding: "0.15rem 0.35rem" }}
                      >
                        BOT 設定
                      </button>
                      <button
                        className="btn-secondary"
                        onClick={() => { setTab("logs"); setLogMonitorId(m.id); }}
                        style={{ fontSize: "0.65rem", padding: "0.15rem 0.35rem" }}
                      >
                        ログ
                      </button>
                      <button
                        className="btn-secondary"
                        onClick={() => handleToggleMonitor(m.id, m.isActive)}
                        style={{ fontSize: "0.65rem", padding: "0.15rem 0.35rem" }}
                      >
                        {m.isActive ? "無効化" : "有効化"}
                      </button>
                      <button
                        className="btn-secondary"
                        onClick={() => handleDeleteMonitor(m.id)}
                        style={{ fontSize: "0.65rem", padding: "0.15rem 0.35rem", color: "#F85149" }}
                      >
                        削除
                      </button>
                    </div>
                  </div>

                  {editingBotMonitorId === m.id && (
                    <div style={{ marginTop: "0.6rem", paddingTop: "0.6rem", borderTop: "1px solid var(--border)" }}>
                      <div style={{ display: "grid", gridTemplateColumns: "1fr 1fr", gap: "0.6rem" }}>
                        <div className="form-group" style={{ gridColumn: "1 / -1" }}>
                          <label>BOT トークン <span style={{ color: "var(--text-muted)" }}>(空欄=変更なし / "-"=クリア)</span></label>
                          <input
                            type="password"
                            autoComplete="new-password"
                            value={botForm.botToken}
                            onChange={(e) => setBotForm({ ...botForm, botToken: e.target.value })}
                            placeholder={m.platform === "slack" ? "xoxb-..." : "Discord Bot Token"}
                          />
                        </div>
                        <div className="form-group">
                          <label>{m.platform === "slack" ? "Workspace ID" : "Guild ID"}</label>
                          <input
                            type="text"
                            value={botForm.botWorkspaceId}
                            onChange={(e) => setBotForm({ ...botForm, botWorkspaceId: e.target.value })}
                          />
                        </div>
                        <div className="form-group">
                          <label>
                            {m.platform === "slack" ? "Signing Secret" : "Public Key"} <span style={{ color: "var(--text-muted)" }}>(空欄=変更なし)</span>
                          </label>
                          <input
                            type="password"
                            autoComplete="new-password"
                            value={botForm.botSigningSecret}
                            onChange={(e) => setBotForm({ ...botForm, botSigningSecret: e.target.value })}
                          />
                        </div>
                        <div className="form-group" style={{ gridColumn: "1 / -1" }}>
                          <label style={{ display: "flex", alignItems: "center", gap: "0.4rem", cursor: "pointer" }}>
                            <input
                              type="checkbox"
                              checked={botForm.captureMessages}
                              onChange={(e) =>
                                setBotForm({ ...botForm, captureMessages: e.target.checked })
                              }
                              style={{ width: "auto" }}
                            />
                            メッセージを取得してログに保存する
                          </label>
                        </div>
                      </div>
                      <div style={{ display: "flex", gap: "0.4rem", marginTop: "0.5rem" }}>
                        <button className="btn-primary" onClick={handleSaveBotConfig} style={{ fontSize: "0.7rem" }}>保存</button>
                        <button
                          className="btn-secondary"
                          onClick={() => setEditingBotMonitorId(null)}
                          style={{ fontSize: "0.7rem" }}
                        >
                          キャンセル
                        </button>
                      </div>
                    </div>
                  )}
                </div>
              ))}
            </div>
          )}
        </div>
      )}

      {/* ─── Chat Logs Tab ──────────────────────────────────── */}
      {tab === "logs" && (
        <div>
          {monitors.length === 0 ? (
            <p style={{ color: "var(--text-muted)", fontSize: "0.8rem" }}>
              監視対象のチャンネルがありません。「監視設定」タブで BOT チャンネルを追加してください。
            </p>
          ) : (
            <>
              <div style={{ display: "flex", gap: "0.5rem", marginBottom: "0.75rem", alignItems: "center", flexWrap: "wrap" }}>
                <label style={{ fontSize: "0.8rem", color: "var(--text-muted)" }}>チャンネル:</label>
                <select
                  value={logMonitorId}
                  onChange={(e) => setLogMonitorId(e.target.value)}
                  style={{ fontSize: "0.8rem", padding: "0.3rem 0.5rem", width: "auto", minWidth: "220px" }}
                >
                  {monitors.map((m) => (
                    <option key={m.id} value={m.id}>
                      [{m.platform}] {m.channelName}
                    </option>
                  ))}
                </select>
                <button
                  className="btn-secondary"
                  onClick={fetchChatLogs}
                  disabled={logsLoading}
                  style={{ fontSize: "0.7rem", padding: "0.25rem 0.5rem" }}
                >
                  再読み込み
                </button>

                <label style={{ fontSize: "0.8rem", color: "var(--text-muted)", marginLeft: "0.75rem" }}>
                  要約対象:
                </label>
                <select
                  value={summaryHours}
                  onChange={(e) => setSummaryHours(parseInt(e.target.value, 10))}
                  style={{ fontSize: "0.8rem", padding: "0.3rem 0.5rem", width: "auto" }}
                >
                  <option value={1}>直近 1 時間</option>
                  <option value={6}>直近 6 時間</option>
                  <option value={24}>直近 24 時間</option>
                  <option value={24 * 7}>直近 1 週間</option>
                  <option value={24 * 30}>直近 30 日</option>
                </select>
                <button
                  className="btn-primary"
                  onClick={handleCreateSummary}
                  disabled={summaryLoading}
                  style={{ fontSize: "0.7rem", padding: "0.25rem 0.6rem" }}
                >
                  {summaryLoading ? "生成中..." : "要約を生成"}
                </button>
              </div>

              <div style={{ display: "flex", gap: "1rem", alignItems: "flex-start" }}>
                {/* Messages Panel */}
                <div className="card" style={{ flex: "1 1 0", minWidth: 0 }}>
                  <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center", marginBottom: "0.5rem" }}>
                    <h3 style={{ fontSize: "0.85rem", color: "var(--text-muted)", margin: 0 }}>
                      チャットログ
                    </h3>
                    <span style={{ fontSize: "0.7rem", color: "var(--text-muted)" }}>
                      {chatMessages.length} / {chatTotal} 件
                    </span>
                  </div>
                  {logsLoading ? (
                    <p style={{ color: "var(--text-muted)", fontSize: "0.8rem" }}>読み込み中...</p>
                  ) : chatMessages.length === 0 ? (
                    <p style={{ color: "var(--text-muted)", fontSize: "0.8rem" }}>
                      このチャンネルのログはまだありません。BOT を連携すると、受信メッセージが自動で記録されます。
                    </p>
                  ) : (
                    <div style={{
                      display: "flex",
                      flexDirection: "column",
                      gap: "0.4rem",
                      maxHeight: "540px",
                      overflowY: "auto",
                    }}>
                      {chatMessages.map((msg) => (
                        <div
                          key={msg.id}
                          style={{
                            padding: "0.4rem 0.6rem",
                            borderLeft: "2px solid var(--border)",
                            background: "var(--bg-surface-2)",
                            borderRadius: "0 4px 4px 0",
                          }}
                        >
                          <div style={{ display: "flex", gap: "0.5rem", alignItems: "baseline", fontSize: "0.72rem" }}>
                            <strong>{msg.authorName || msg.authorId}</strong>
                            <span style={{ color: "var(--text-muted)" }}>
                              {new Date(msg.postedAt).toLocaleString("ja-JP")}
                            </span>
                          </div>
                          <div style={{ fontSize: "0.8rem", whiteSpace: "pre-wrap", marginTop: "0.15rem" }}>
                            {msg.text}
                          </div>
                        </div>
                      ))}
                    </div>
                  )}
                </div>

                {/* Summaries Panel */}
                <div className="card" style={{ flex: "0 0 380px", maxWidth: "380px" }}>
                  <h3 style={{ fontSize: "0.85rem", color: "var(--text-muted)", marginBottom: "0.5rem" }}>
                    要約
                  </h3>
                  {summaries.length === 0 ? (
                    <p style={{ color: "var(--text-muted)", fontSize: "0.75rem" }}>
                      まだ要約がありません。上の「要約を生成」ボタンで作成できます。
                    </p>
                  ) : (
                    <div style={{ display: "flex", flexDirection: "column", gap: "0.6rem", maxHeight: "540px", overflowY: "auto" }}>
                      {summaries.map((s) => {
                        const h = parseHighlights(s.highlights);
                        return (
                          <div
                            key={s.id}
                            style={{
                              borderLeft: "2px solid var(--accent)",
                              paddingLeft: "0.5rem",
                            }}
                          >
                            <div style={{ fontSize: "0.7rem", color: "var(--text-muted)", display: "flex", justifyContent: "space-between" }}>
                              <span>
                                {new Date(s.periodStart).toLocaleString("ja-JP")} 〜{" "}
                                {new Date(s.periodEnd).toLocaleString("ja-JP")}
                              </span>
                              <button
                                onClick={() => handleDeleteSummary(s.id)}
                                className="btn-secondary"
                                style={{ fontSize: "0.6rem", padding: "0 0.3rem", color: "#F85149" }}
                              >
                                削除
                              </button>
                            </div>
                            <div style={{ fontSize: "0.78rem", whiteSpace: "pre-wrap", marginTop: "0.2rem" }}>
                              {s.summary}
                            </div>
                            {h && h.topKeywords.length > 0 && (
                              <div style={{ fontSize: "0.7rem", marginTop: "0.3rem" }}>
                                <span style={{ color: "var(--text-muted)" }}>キーワード: </span>
                                {h.topKeywords.slice(0, 6).map((k, i) => (
                                  <span
                                    key={i}
                                    style={{
                                      display: "inline-block",
                                      background: "var(--bg-surface-2)",
                                      padding: "0.05rem 0.3rem",
                                      borderRadius: "2px",
                                      marginRight: "0.25rem",
                                      marginBottom: "0.15rem",
                                    }}
                                  >
                                    {k.keyword} <span style={{ color: "var(--text-muted)" }}>×{k.count}</span>
                                  </span>
                                ))}
                              </div>
                            )}
                            <div style={{ fontSize: "0.65rem", color: "var(--text-muted)", marginTop: "0.25rem" }}>
                              {s.messageCount} 件のメッセージを集約
                            </div>
                          </div>
                        );
                      })}
                    </div>
                  )}
                </div>
              </div>
            </>
          )}
        </div>
      )}

      {/* ─── Analyze Tab ────────────────────────────────────── */}
      {tab === "analyze" && (
        <div>
          <div className="card" style={{ marginBottom: "1rem" }}>
            <h3 style={{ fontSize: "0.85rem", marginBottom: "0.75rem", color: "var(--text-muted)" }}>
              テキスト解析プレビュー
            </h3>
            <p style={{ fontSize: "0.75rem", color: "var(--text-muted)", marginBottom: "0.5rem" }}>
              Slack/Discord のメッセージテキストを入力すると、タスク自動生成の解析結果をプレビューできます。
            </p>
            <div className="form-group">
              <textarea
                value={analyzeText}
                onChange={(e) => setAnalyzeText(e.target.value)}
                placeholder="例: @田中 ログイン画面のバグを今日中に修正してください"
                rows={3}
                style={{ fontFamily: "monospace" }}
              />
            </div>
            <button className="btn-primary" onClick={handleAnalyze} style={{ fontSize: "0.75rem" }}>
              解析
            </button>
          </div>

          {analyzeResult && (
            <div className="card">
              <h3 style={{ fontSize: "0.85rem", marginBottom: "0.5rem", color: "var(--text-muted)" }}>解析結果</h3>
              <div style={{ fontSize: "0.8rem", display: "flex", flexDirection: "column", gap: "0.3rem" }}>
                <div>
                  <strong>タスク生成:</strong>{" "}
                  <span style={{ color: analyzeResult.shouldCreateTask ? "#3FB950" : "#8B949E" }}>
                    {analyzeResult.shouldCreateTask ? "はい" : "いいえ"}
                  </span>
                </div>
                {analyzeResult.shouldCreateTask ? (
                  <>
                    <div><strong>タイトル:</strong> {analyzeResult.title}</div>
                    <div><strong>優先度:</strong> {PRIORITY_LABELS[analyzeResult.priority] || analyzeResult.priority}</div>
                    <div><strong>アサインヒント:</strong> {analyzeResult.assigneeHint || "なし"}</div>
                    <div><strong>納期ヒント:</strong> {analyzeResult.dueDateHint ? analyzeResult.dueDateHint.slice(0, 10) : "なし"}</div>
                    <div><strong>信頼度:</strong> {analyzeResult.confidence * 100}%</div>
                  </>
                ) : null}
                <div><strong>判定理由:</strong> {analyzeResult.reasoning}</div>
                {analyzeResult.isCompletion ? (
                  <div style={{ color: "#3FB950" }}><strong>完了キーワード検出</strong> — 既存タスクのステータス更新が行われます</div>
                ) : null}
              </div>
            </div>
          )}
        </div>
      )}
    </div>
  );
}
