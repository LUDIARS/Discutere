/**
 * 結論リスト + 論述データ閲覧 (#66)。
 *
 * 収束した議論 (gap status = closed/converged) の【収束】まとめを「結論」として一覧化し、
 * 選んだ結論の裏にある論述データ (議論ログ / 止揚ストック / 高評価意見) を辿れるようにする。
 */

import type { createCore } from "../core/index.js";
import { getOpinionScore } from "../discord-hook/reactions.js";

type Core = ReturnType<typeof createCore>;

export interface ConclusionSummary {
  gapId: string;
  sessionId: string;
  title: string;
  /** 【収束】まとめ本文 (プレフィックス除去済)。 無ければ null。 */
  conclusion: string | null;
  utteranceCount: number;
  aufhebungCount: number;
  updatedAt: number;
}

export interface ConclusionDetail extends ConclusionSummary {
  aufhebungen: string[];
  topOpinions: Array<{ speaker: string; content: string; score: number }>;
  transcript: Array<{ speaker: string; content: string }>;
}

const CONVERGE_PREFIX = "【収束】";

/** 収束済み議論の結論を新しい順に一覧する。 */
export function listConclusions(core: Core, workspaceId: string, limit = 20): ConclusionSummary[] {
  const raw = core.client.raw;
  const rows = raw
    .prepare(
      `SELECT g.id AS gapId, g.title AS title, g.updated_at AS updatedAt, s.id AS sessionId
         FROM design_gaps g
         JOIN sessions s
           ON s.workspace_id = g.workspace_id
          AND s.title = 'discussion-of-gap:' || g.id
        WHERE g.workspace_id = ?
          AND g.status IN ('closed', 'converged')
        ORDER BY g.updated_at DESC
        LIMIT ?`
    )
    .all(workspaceId, limit) as Array<{
    gapId: string;
    title: string;
    updatedAt: number;
    sessionId: string;
  }>;
  return rows.map((r) => ({
    gapId: r.gapId,
    sessionId: r.sessionId,
    title: r.title,
    conclusion: convergeText(core, r.sessionId),
    utteranceCount: utteranceCount(core, r.sessionId),
    aufhebungCount: aufhebungCount(core, r.gapId),
    updatedAt: r.updatedAt,
  }));
}

/** 1 件の結論の裏にある論述データを取り出す。 gap が無ければ null。 */
export function getConclusionDetail(
  core: Core,
  workspaceId: string,
  gapId: string
): ConclusionDetail | null {
  const raw = core.client.raw;
  const gap = raw
    .prepare("SELECT id, title, updated_at FROM design_gaps WHERE workspace_id = ? AND id = ?")
    .get(workspaceId, gapId) as { id: string; title: string; updated_at: number } | undefined;
  if (!gap) return null;
  const session = raw
    .prepare(
      "SELECT id FROM sessions WHERE workspace_id = ? AND title = ? ORDER BY started_at DESC LIMIT 1"
    )
    .get(workspaceId, `discussion-of-gap:${gapId}`) as { id: string } | undefined;
  const sessionId = session?.id ?? "";

  return {
    gapId,
    sessionId,
    title: gap.title,
    conclusion: sessionId ? convergeText(core, sessionId) : null,
    utteranceCount: sessionId ? utteranceCount(core, sessionId) : 0,
    aufhebungCount: aufhebungCount(core, gapId),
    updatedAt: gap.updated_at,
    aufhebungen: listAufhebung(core, gapId),
    topOpinions: sessionId ? topOpinions(core, sessionId, 5) : [],
    transcript: sessionId ? transcript(core, sessionId) : [],
  };
}

function convergeText(core: Core, sessionId: string): string | null {
  const row = core.client.raw
    .prepare(
      "SELECT raw_content FROM utterances WHERE session_id = ? AND raw_content LIKE ? ORDER BY posted_at DESC LIMIT 1"
    )
    .get(sessionId, `${CONVERGE_PREFIX}%`) as { raw_content: string } | undefined;
  if (!row) return null;
  return row.raw_content.slice(CONVERGE_PREFIX.length).trim();
}

function utteranceCount(core: Core, sessionId: string): number {
  const row = core.client.raw
    .prepare("SELECT COUNT(*) AS n FROM utterances WHERE session_id = ?")
    .get(sessionId) as { n: number };
  return row.n;
}

function aufhebungCount(core: Core, gapId: string): number {
  const row = core.client.raw
    .prepare("SELECT COUNT(*) AS n FROM aufhebung_stock WHERE gap_id = ?")
    .get(gapId) as { n: number } | undefined;
  return row?.n ?? 0;
}

function listAufhebung(core: Core, gapId: string): string[] {
  const rows = core.client.raw
    .prepare("SELECT summary FROM aufhebung_stock WHERE gap_id = ? ORDER BY created_at ASC")
    .all(gapId) as Array<{ summary: string }>;
  return rows.map((r) => r.summary);
}

function transcript(core: Core, sessionId: string): Array<{ speaker: string; content: string }> {
  const rows = core.client.raw
    .prepare(
      "SELECT speaker_id, raw_content FROM utterances WHERE session_id = ? ORDER BY posted_at ASC"
    )
    .all(sessionId) as Array<{ speaker_id: string | null; raw_content: string }>;
  return rows.map((r) => ({ speaker: speakerLabel(r.speaker_id), content: r.raw_content }));
}

function topOpinions(
  core: Core,
  sessionId: string,
  limit: number
): Array<{ speaker: string; content: string; score: number }> {
  const raw = core.client.raw;
  const rows = raw
    .prepare(
      "SELECT id, speaker_id, raw_content FROM utterances WHERE session_id = ? AND speaker_id LIKE 'persona:%'"
    )
    .all(sessionId) as Array<{ id: string; speaker_id: string; raw_content: string }>;
  return rows
    .map((u) => ({
      speaker: speakerLabel(u.speaker_id),
      content: u.raw_content,
      score: getOpinionScore(raw, u.id),
    }))
    .filter((u) => u.score > 0)
    .sort((a, b) => b.score - a.score)
    .slice(0, limit);
}

function speakerLabel(speakerId: string | null): string {
  if (!speakerId) return "参加者";
  if (speakerId.startsWith("persona:")) return speakerId.slice("persona:".length);
  if (speakerId.startsWith("ext:")) return "外部の声";
  return "人間";
}
