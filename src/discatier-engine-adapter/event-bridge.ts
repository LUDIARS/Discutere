/**
 * Discatier events table → persona-engine.fireEvent への配線 (PR-D).
 *
 * Discatier Core は events を SQLite に append するだけで「リスナー」 機構を持たない。
 * ここで定期 polling して新着 event を持続 ID 監視し、 persona-engine に流す。
 *
 * - 1 秒 (default) で events table を `SELECT * WHERE created_at > lastSeen` で scan
 * - event.kind を持って engine.fireEvent({kind, sessionId, payload}) を発火
 * - sessionId は payload.sessionId か、 gap-related なら gap-id 紐付け session を解決
 * - 終結イベント (HypothesisIntegrated / HypothesisRejected) は engine.resetSession で
 *   safety cap を解放
 *
 * 議論セッション概念 (1-2):
 * - design gap 1 つに対し 1 つの persistent session を作って継続
 * - session.title = "discussion-of-gap:<gapId>"
 * - 既存 session があれば再利用、 なければ作成
 */

import type Database from "better-sqlite3";

import type { createCore } from "../core/index.js";
import type { PersonaEngineHandle } from "../persona-engine/index.js";

type Core = ReturnType<typeof createCore>;

interface DiscatierEventRow {
  id: string;
  event_type: string;
  payload_json: string;
  created_at: number;
}

export interface EventBridgeOptions {
  workspaceId: string;
  /** polling 周期 ms (default 1000) */
  pollMs?: number;
  /** 終結とみなす event types (default: HypothesisIntegrated / HypothesisRejected) */
  closingEventTypes?: string[];
  /** ログ出力 (default: console.warn) */
  onError?: (err: unknown, ev?: DiscatierEventRow) => void;
}

export interface EventBridgeHandle {
  start(): void;
  stop(): void;
  /** test 用: 現在の lastSeen を返す */
  lastSeenAt(): number;
  /** test 用: 手動 1 回 poll */
  pollOnce(): Promise<number>;
}

const DEFAULT_POLL_MS = 1000;
const DEFAULT_CLOSING = [
  "HypothesisIntegrated",
  "HypothesisRejected",
];

export function createEventBridge(
  core: Core,
  engine: PersonaEngineHandle,
  options: EventBridgeOptions
): EventBridgeHandle {
  const pollMs = options.pollMs ?? DEFAULT_POLL_MS;
  const closing = new Set(options.closingEventTypes ?? DEFAULT_CLOSING);
  const onError = options.onError ?? ((err) => console.warn("[event-bridge]", err));

  // 起動時の最新 event を起点に (= 過去 event を流し直さない)
  const initial = core.client.raw
    .prepare("SELECT COALESCE(MAX(created_at), 0) AS max_ts FROM events")
    .get() as { max_ts: number };
  let lastSeen = initial.max_ts;

  let timer: NodeJS.Timeout | null = null;
  let stopped = false;
  let polling = false;

  async function pollOnce(): Promise<number> {
    if (polling) return 0;
    polling = true;
    let dispatched = 0;
    try {
      const rows = core.client.raw
        .prepare(
          "SELECT id, event_type, payload_json, created_at FROM events WHERE created_at > ? ORDER BY created_at ASC, id ASC"
        )
        .all(lastSeen) as DiscatierEventRow[];
      for (const row of rows) {
        try {
          await dispatchOne(row);
          dispatched += 1;
          if (row.created_at > lastSeen) {
            lastSeen = row.created_at;
          }
        } catch (err) {
          onError(err, row);
        }
      }
    } finally {
      polling = false;
    }
    return dispatched;
  }

  async function dispatchOne(row: DiscatierEventRow): Promise<void> {
    const payload = parsePayload(row.payload_json);
    if (payload && typeof payload.workspaceId === "string" && payload.workspaceId !== options.workspaceId) {
      // 別 workspace の event は無視
      return;
    }

    const sessionId = resolveSessionId(row.event_type, payload);

    await engine.fireEvent({
      kind: row.event_type,
      sessionId,
      payload,
    });

    // 終結 event なら session カウンタ解放
    if (closing.has(row.event_type) && sessionId) {
      engine.resetSession(sessionId);
    }
  }

  function resolveSessionId(
    eventType: string,
    payload: Record<string, unknown> | null
  ): string | null {
    if (!payload) return null;
    // 1) payload に sessionId が含まれていれば最優先
    if (typeof payload.sessionId === "string" && payload.sessionId.length > 0) {
      return payload.sessionId;
    }
    // 2) DesignGap 関連: gapId に紐付く 「議論 session」 を引き当て (なければ作る)
    if (
      eventType === "DesignGapDetected" ||
      eventType === "DesignGapUpdated"
    ) {
      const gapId = typeof payload.id === "string" ? payload.id : null;
      if (gapId) {
        return ensureGapDiscussionSession(gapId);
      }
    }
    // 3) Hypothesis 関連: hypothesis 経由で gap を引き、 さらに gap session に紐付け
    if (
      eventType === "HypothesisCreated" ||
      eventType === "HypothesisProposed" ||
      eventType === "HypothesisValidated" ||
      eventType === "HypothesisIntegrated" ||
      eventType === "HypothesisRejected" ||
      eventType === "HypothesisUpdated"
    ) {
      const hid = typeof payload.id === "string" ? payload.id : null;
      const gapIdInline = typeof payload.designGapId === "string" ? payload.designGapId : null;
      const gapId = gapIdInline ?? (hid ? lookupGapIdForHypothesis(hid) : null);
      if (gapId) {
        return ensureGapDiscussionSession(gapId);
      }
    }
    return null;
  }

  function lookupGapIdForHypothesis(hypothesisId: string): string | null {
    const row = core.client.raw
      .prepare("SELECT design_gap_id FROM hypotheses WHERE id = ?")
      .get(hypothesisId) as { design_gap_id: string | null } | undefined;
    return row?.design_gap_id ?? null;
  }

  function ensureGapDiscussionSession(gapId: string): string {
    const title = `discussion-of-gap:${gapId}`;
    const existing = core.client.raw
      .prepare(
        "SELECT id FROM sessions WHERE workspace_id = ? AND title = ? ORDER BY started_at DESC LIMIT 1"
      )
      .get(options.workspaceId, title) as { id: string } | undefined;
    if (existing) return existing.id;
    return core.repos.session.create({
      workspaceId: options.workspaceId,
      title,
      startedAt: Date.now(),
      scene: `gap:${gapId}`,
    } as never);
  }

  return {
    start(): void {
      if (timer) return;
      stopped = false;
      timer = setInterval(() => {
        if (stopped) return;
        pollOnce().catch(onError);
      }, pollMs);
    },
    stop(): void {
      stopped = true;
      if (timer) {
        clearInterval(timer);
        timer = null;
      }
    },
    lastSeenAt(): number {
      return lastSeen;
    },
    pollOnce,
  };
}

function parsePayload(raw: string): Record<string, unknown> | null {
  try {
    const v = JSON.parse(raw);
    if (v && typeof v === "object" && !Array.isArray(v)) {
      return v as Record<string, unknown>;
    }
  } catch {
    /* ignore */
  }
  return null;
}
