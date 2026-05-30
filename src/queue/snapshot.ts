/**
 * 議論キューのスナップショット生成 (議論の展開 / 現状積まれているキューの可視化).
 *
 * 「今どんな議論が走っていて、何が未処理で積まれているか」を 1 つの構造体にまとめる。
 *   - activeSessions: discord-bound でまだ閉じていない議論 session (発話数 / 最終発話 / 発火数)
 *   - openGaps:       未解決の DesignGap (= AI が仮説を出すべき対象としてキューに積まれている)
 *   - pendingHypotheses: 未統合・未棄却の Hypothesis (= 検証待ちのキュー)
 *
 * Discatier Core (kuzu=sqlite) を読み取り専用で参照し、発火数のみ persona-engine db を見る。
 * Transport / engine 本体に依存しない純データ層 (SRP)。
 */

import type Database from "better-sqlite3";

export interface QueueSessionEntry {
  sessionId: string;
  title: string;
  scene: string | null;
  guildId: string | null;
  channelId: string | null;
  startedAt: number;
  utteranceCount: number;
  lastUtteranceAt: number | null;
  fireCount: number;
}

export interface QueueGapEntry {
  id: string;
  title: string;
  status: string | null;
}

export interface QueueHypothesisEntry {
  id: string;
  statement: string;
  status: string | null;
  designGapId: string | null;
}

export interface QueueSnapshot {
  workspaceId: string;
  generatedAt: number;
  activeSessions: QueueSessionEntry[];
  openGaps: QueueGapEntry[];
  pendingHypotheses: QueueHypothesisEntry[];
  totals: {
    activeSessions: number;
    openGaps: number;
    pendingHypotheses: number;
  };
}

const SESSION_LIMIT = 50;
const GAP_LIMIT = 100;
const HYPOTHESIS_LIMIT = 100;

/**
 * @param coreDb Discatier Core の sqlite (createCore().client.raw)
 * @param peDb   persona-engine の sqlite (session_rule_fires 用)。null なら fireCount=0
 */
export function buildQueueSnapshot(
  coreDb: Database.Database,
  peDb: Database.Database | null,
  workspaceId: string
): QueueSnapshot {
  const sessionRows = coreDb
    .prepare(
      `SELECT id, title, scene, started_at
         FROM sessions
        WHERE workspace_id = ? AND scene LIKE 'discord:%' AND ended_at IS NULL
        ORDER BY started_at DESC
        LIMIT ?`
    )
    .all(workspaceId, SESSION_LIMIT) as Array<{
    id: string;
    title: string;
    scene: string | null;
    started_at: number;
  }>;

  const utterStmt = coreDb.prepare(
    "SELECT COUNT(*) AS c, MAX(posted_at) AS last FROM utterances WHERE session_id = ?"
  );
  const fireStmt = peDb
    ? peDb.prepare("SELECT COALESCE(SUM(count), 0) AS total FROM session_rule_fires WHERE session_id = ?")
    : null;

  const activeSessions: QueueSessionEntry[] = sessionRows.map((s) => {
    const u = utterStmt.get(s.id) as { c: number; last: number | null };
    const fire = fireStmt ? (fireStmt.get(s.id) as { total: number }).total : 0;
    const { guildId, channelId } = parseScene(s.scene);
    return {
      sessionId: s.id,
      title: s.title,
      scene: s.scene,
      guildId,
      channelId,
      startedAt: s.started_at,
      utteranceCount: u.c,
      lastUtteranceAt: u.last,
      fireCount: fire,
    };
  });

  const openGaps = (
    coreDb
      .prepare(
        `SELECT id, title, status
           FROM design_gaps
          WHERE workspace_id = ?
            AND (status IS NULL OR status NOT IN ('resolved', 'closed', 'rejected'))
          ORDER BY created_at DESC
          LIMIT ?`
      )
      .all(workspaceId, GAP_LIMIT) as Array<{ id: string; title: string; status: string | null }>
  ).map((g) => ({ id: g.id, title: g.title, status: g.status }));

  const pendingHypotheses = (
    coreDb
      .prepare(
        `SELECT id, statement, status, design_gap_id
           FROM hypotheses
          WHERE workspace_id = ?
            AND integrated = 0
            AND (status IS NULL OR status NOT IN ('rejected', 'integrated'))
          ORDER BY created_at DESC
          LIMIT ?`
      )
      .all(workspaceId, HYPOTHESIS_LIMIT) as Array<{
      id: string;
      statement: string;
      status: string | null;
      design_gap_id: string | null;
    }>
  ).map((h) => ({
    id: h.id,
    statement: h.statement,
    status: h.status,
    designGapId: h.design_gap_id,
  }));

  return {
    workspaceId,
    generatedAt: Date.now(),
    activeSessions,
    openGaps,
    pendingHypotheses,
    totals: {
      activeSessions: activeSessions.length,
      openGaps: openGaps.length,
      pendingHypotheses: pendingHypotheses.length,
    },
  };
}

function parseScene(scene: string | null): { guildId: string | null; channelId: string | null } {
  if (!scene || !scene.startsWith("discord:")) return { guildId: null, channelId: null };
  const [guildId, channelId] = scene.slice("discord:".length).split("/");
  return { guildId: guildId || null, channelId: channelId || null };
}

/** slash / ログ向けの短い人間可読サマリ (Discord 2000 字制限を意識して上位のみ) */
export function formatQueueText(snap: QueueSnapshot): string {
  const lines: string[] = [];
  lines.push(
    `🧭 **議論キュー** (ws=\`${snap.workspaceId}\`) — ` +
      `session ${snap.totals.activeSessions} / gap ${snap.totals.openGaps} / 仮説 ${snap.totals.pendingHypotheses}`
  );
  if (snap.activeSessions.length > 0) {
    lines.push("");
    lines.push("**進行中の議論**");
    for (const s of snap.activeSessions.slice(0, 8)) {
      lines.push(
        `• \`${s.channelId ?? "?"}\` — 発話 ${s.utteranceCount} / 発火 ${s.fireCount}`
      );
    }
  }
  if (snap.openGaps.length > 0) {
    lines.push("");
    lines.push("**未処理の DesignGap (仮説待ち)**");
    for (const g of snap.openGaps.slice(0, 6)) {
      lines.push(`• ${truncate(g.title, 80)}`);
    }
  }
  if (snap.pendingHypotheses.length > 0) {
    lines.push("");
    lines.push("**検証待ちの仮説**");
    for (const h of snap.pendingHypotheses.slice(0, 6)) {
      lines.push(`• ${truncate(h.statement, 80)} (${h.status ?? "open"})`);
    }
  }
  if (
    snap.totals.activeSessions === 0 &&
    snap.totals.openGaps === 0 &&
    snap.totals.pendingHypotheses === 0
  ) {
    lines.push("");
    lines.push("_キューは空です_");
  }
  return truncate(lines.join("\n"), 1900);
}

function truncate(s: string, max: number): string {
  return s.length <= max ? s : s.slice(0, max - 1) + "…";
}
