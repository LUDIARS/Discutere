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
import { recordHypothesisOutcome } from "../persona-engine/index.js";

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

    // 人間の発言 → 進行中の議論に最優先で素早く応答させる (人間 > AI)。
    // UtteranceCreated のうち speaker が人間 (persona:/ext: 以外) のものを拾い、
    // 同チャンネルの open な議論 session にミラーしてから HumanUtterance を発火する。
    if (row.event_type === "UtteranceCreated") {
      await handleHumanUtterance(payload);
    }

    // 終結 event なら session カウンタ解放 + learning loop (#3-1)
    if (closing.has(row.event_type)) {
      if (sessionId) engine.resetSession(sessionId);
      const hypothesisId = typeof payload?.id === "string" ? payload.id : null;
      if (hypothesisId) {
        const outcome =
          row.event_type === "HypothesisIntegrated" ? "integrated" : "rejected";
        const statement = typeof payload?.statement === "string" ? payload.statement : undefined;
        try {
          recordHypothesisOutcome({
            personas: engine.personas,
            rules: engine.rules,
            hypothesisId,
            outcome,
            statement,
          });
        } catch (err) {
          onError(err, row);
        }
      }
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

  /** speaker が「人間」 か (persona:/ext: でも空でもない = Discord ユーザ)。 */
  function isHumanSpeaker(speakerId: unknown): speakerId is string {
    return (
      typeof speakerId === "string" &&
      speakerId.length > 0 &&
      !speakerId.startsWith("persona:") &&
      !speakerId.startsWith("ext:")
    );
  }

  /**
   * 人間の発言を、 同チャンネル (scene) で進行中の議論 session にミラーして
   * HumanUtterance イベントを発火する (人間優先・即応のトリガ)。
   * - ミラーで人間が議論の一次参加者になる (収束まとめ / persona の文脈に乗る)。
   * - source が既に discussion-of-gap session の場合は no-op (ミラーの再帰防止)。
   * - 同チャンネルに open な議論が無ければ何もしない (議論の起動は auto-classifier 側)。
   * - トランスポートは Discord (`discord:`) と Web チャット (`web:`) を等価に扱う。
   */
  async function handleHumanUtterance(payload: Record<string, unknown> | null): Promise<void> {
    if (!payload) return;
    const speakerId = payload.speakerId;
    if (!isHumanSpeaker(speakerId)) return;
    const rawContent = typeof payload.rawContent === "string" ? payload.rawContent : "";
    if (!rawContent.trim()) return;
    const srcSessionId = typeof payload.sessionId === "string" ? payload.sessionId : "";
    if (!srcSessionId) return;

    const src = core.client.raw
      .prepare("SELECT title, scene FROM sessions WHERE id = ?")
      .get(srcSessionId) as { title: string; scene: string | null } | undefined;
    if (!src) return;
    // 既に議論 session 内の発言 (= ミラー済 or persona 議論場) は再処理しない。
    if (src.title.startsWith("discussion-of-gap:")) return;
    // トランスポート bound な scene (Discord / Web) の入口発話だけミラーする。
    if (!isBoundScene(src.scene)) return;

    // 同じチャンネル (scene) の open な議論 session を引く。
    const discussion = core.client.raw
      .prepare(
        `SELECT s.id AS id, s.title AS title
           FROM sessions s
           JOIN design_gaps g
             ON g.id = SUBSTR(s.title, LENGTH('discussion-of-gap:') + 1)
          WHERE s.workspace_id = ?
            AND s.title LIKE 'discussion-of-gap:%'
            AND s.scene = ?
            AND (g.status IS NULL OR g.status NOT IN ('closed', 'converged'))
          ORDER BY s.started_at DESC LIMIT 1`
      )
      .get(options.workspaceId, src.scene) as { id: string; title: string } | undefined;
    if (!discussion) return;

    // 人間発言を議論 session にミラー (= 議論の一次参加者として記録)。
    core.repos.utterance.create({
      workspaceId: options.workspaceId,
      sessionId: discussion.id,
      speakerId,
      rawContent,
      postedAt: Date.now(),
    });

    // HumanUtterance を発火 → respond-to-human ルールが最優先で即応する。
    await engine.fireEvent({
      kind: "HumanUtterance",
      sessionId: discussion.id,
      payload: { ...payload, sessionId: discussion.id, humanSpeakerId: speakerId },
    });
  }

  function lookupGapIdForHypothesis(hypothesisId: string): string | null {
    const row = core.client.raw
      .prepare("SELECT design_gap_id FROM hypotheses WHERE id = ?")
      .get(hypothesisId) as { design_gap_id: string | null } | undefined;
    return row?.design_gap_id ?? null;
  }

  function ensureGapDiscussionSession(gapId: string): string {
    const title = `discussion-of-gap:${gapId}`;
    const scene = resolveGapDiscussionScene(gapId);
    const existing = core.client.raw
      .prepare(
        "SELECT id, scene FROM sessions WHERE workspace_id = ? AND title = ? ORDER BY started_at DESC LIMIT 1"
      )
      .get(options.workspaceId, title) as { id: string; scene: string | null } | undefined;
    if (existing) {
      if (isBoundScene(scene) && existing.scene !== scene) {
        core.client.raw
          .prepare("UPDATE sessions SET scene = ?, updated_at = ? WHERE id = ?")
          .run(scene, Date.now(), existing.id);
      }
      return existing.id;
    }
    return core.repos.session.create({
      workspaceId: options.workspaceId,
      title,
      startedAt: Date.now(),
      scene,
    } as never);
  }

  function resolveGapDiscussionScene(gapId: string): string {
    const gap = core.client.raw
      .prepare("SELECT evidence_json FROM design_gaps WHERE id = ?")
      .get(gapId) as { evidence_json: string | null } | undefined;
    const evidence = parsePayload(gap?.evidence_json ?? "");
    const evidenceSessionId =
      typeof evidence?.sessionId === "string" && evidence.sessionId.length > 0
        ? evidence.sessionId
        : null;
    if (evidenceSessionId) {
      const sourceSession = core.client.raw
        .prepare("SELECT scene FROM sessions WHERE id = ?")
        .get(evidenceSessionId) as { scene: string | null } | undefined;
      // 入口発話の session が transport bound (Discord / Web) ならその scene を継承する。
      if (isBoundScene(sourceSession?.scene)) return sourceSession.scene;
    }
    const guildId =
      typeof evidence?.guildId === "string" && evidence.guildId.length > 0
        ? evidence.guildId
        : null;
    const channelId =
      typeof evidence?.channelId === "string" && evidence.channelId.length > 0
        ? evidence.channelId
        : null;
    // Web チャット由来 (guildId="web") は web:<room> に、 それ以外は discord:<guild>/<channel>。
    if (guildId === "web" && channelId) return `web:${channelId}`;
    if (guildId && channelId) return `discord:${guildId}/${channelId}`;
    return `gap:${gapId}`;
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

/**
 * transport bound な scene か (Discord = `discord:` / Web チャット = `web:`)。
 * これらの scene は「進行中議論 session へ scene を継承」「入口の人間発話を議論にミラー」
 * の対象になる。 headless (`gap:<id>` 等) は対象外。
 */
function isBoundScene(scene: string | null | undefined): scene is string {
  return typeof scene === "string" && (scene.startsWith("discord:") || scene.startsWith("web:"));
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
