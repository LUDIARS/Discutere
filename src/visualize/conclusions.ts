/**
 * 結論リスト + 論述データ閲覧 (#66)。
 *
 * 収束した議論 (gap status = closed/converged) の【収束】まとめを「結論」として一覧化し、
 * 選んだ結論の裏にある論述データ (議論ログ / 止揚ストック / 高評価意見) を辿れるようにする。
 */

import type { createCore } from "../core/index.js";
import { getOpinionScore } from "../discord-hook/reactions.js";
import type { AttributionStore } from "../crawler/sources/attribution-store.js";
import { maskedPersonaLabel } from "../crawler/sources/persona.js";

type Core = ReturnType<typeof createCore>;

/** 発話の出所バッジ (§6 — source/sourceUrl は開示、 内部発話は null)。 */
export interface SourceRef {
  source: string;
  sourceUrl: string;
}

/**
 * 結論の出自。 "gap" = 旧 auto-discussion (design_gaps)、 "flow" = 新フロー (flow_conclusion)。
 * 学習ビューは両者をマージして「結論」タブに並べる (新フロー結論統合)。
 */
export type ConclusionKind = "gap" | "flow";

export interface ConclusionSummary {
  /**
   * 結論の一意キー。 旧フローは design_gap の id、 新フローは "flow:<sessionId>"。
   * 詳細取得 (/learning/conclusion?gap=) と md エクスポートはこの id でルートする。
   */
  gapId: string;
  kind: ConclusionKind;
  sessionId: string;
  title: string;
  /** 【収束】まとめ本文 (プレフィックス除去済)。 無ければ null。 */
  conclusion: string | null;
  utteranceCount: number;
  aufhebungCount: number;
  updatedAt: number;
  /**
   * 話題のサイズ — 議論ボリューム (発話数)。結論キャッシュ (conclusion_cache) で
   * write-through 更新する値。live 経路 (KG 直読) では undefined。
   */
  discussionVolume?: number;
  /**
   * 話題のサイズ — 話題に紐づく外部クロール材料 (steam/youtube/niconico 等) の件数。
   * KG を引く重い計算なので別途 (build:conclusion-cache) で算出しキャッシュする。
   * -1 = 未算出、undefined = キャッシュ非経由。
   */
  materialCount?: number;
}

export interface ConclusionDetail extends ConclusionSummary {
  aufhebungen: string[];
  topOpinions: Array<{ speaker: string; content: string; score: number; source: string | null; sourceUrl: string | null }>;
  transcript: Array<{ speaker: string; content: string; source: string | null; sourceUrl: string | null }>;
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
    kind: "gap" as const,
    sessionId: r.sessionId,
    title: r.title,
    conclusion: convergeText(core, r.sessionId),
    utteranceCount: utteranceCount(core, r.sessionId),
    aufhebungCount: aufhebungCount(core, r.gapId),
    updatedAt: r.updatedAt,
  }));
}

/**
 * 1 件の結論の裏にある論述データを取り出す。 gap が無ければ null。
 * attribution を渡すと発話ごとに出所 (source / sourceUrl) を付与する (§6 露出制御)。
 */
export function getConclusionDetail(
  core: Core,
  workspaceId: string,
  gapId: string,
  attribution?: AttributionStore | null
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
    kind: "gap" as const,
    sessionId,
    title: gap.title,
    conclusion: sessionId ? convergeText(core, sessionId) : null,
    utteranceCount: sessionId ? utteranceCount(core, sessionId) : 0,
    aufhebungCount: aufhebungCount(core, gapId),
    updatedAt: gap.updated_at,
    aufhebungen: listAufhebung(core, gapId),
    topOpinions: sessionId ? topOpinions(core, sessionId, 5, attribution) : [],
    transcript: sessionId ? transcript(core, sessionId, attribution) : [],
  };
}

/** attribution-store から発話の出所を引く (個人マスクとは別レイヤー、 §6)。 無ければ null。 */
function sourceRef(attribution: AttributionStore | null | undefined, utteranceId: string): SourceRef | null {
  if (!attribution) return null;
  const a = attribution.get(utteranceId);
  return a ? { source: a.source, sourceUrl: a.sourceUrl } : null;
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

function transcript(
  core: Core,
  sessionId: string,
  attribution?: AttributionStore | null
): Array<{ speaker: string; content: string; source: string | null; sourceUrl: string | null }> {
  const rows = core.client.raw
    .prepare(
      "SELECT id, speaker_id, raw_content FROM utterances WHERE session_id = ? ORDER BY posted_at ASC"
    )
    .all(sessionId) as Array<{ id: string; speaker_id: string | null; raw_content: string }>;
  return rows.map((r) => {
    const ref = sourceRef(attribution, r.id);
    return {
      speaker: speakerLabel(r.speaker_id),
      content: r.raw_content,
      source: ref?.source ?? null,
      sourceUrl: ref?.sourceUrl ?? null,
    };
  });
}

function topOpinions(
  core: Core,
  sessionId: string,
  limit: number,
  attribution?: AttributionStore | null
): Array<{ speaker: string; content: string; score: number; source: string | null; sourceUrl: string | null }> {
  const raw = core.client.raw;
  const rows = raw
    .prepare(
      "SELECT id, speaker_id, raw_content FROM utterances WHERE session_id = ? AND speaker_id LIKE 'persona:%'"
    )
    .all(sessionId) as Array<{ id: string; speaker_id: string; raw_content: string }>;
  return rows
    .map((u) => {
      const ref = sourceRef(attribution, u.id);
      return {
        speaker: speakerLabel(u.speaker_id),
        content: u.raw_content,
        score: getOpinionScore(raw, u.id),
        source: ref?.source ?? null,
        sourceUrl: ref?.sourceUrl ?? null,
      };
    })
    .filter((u) => u.score > 0)
    .sort((a, b) => b.score - a.score)
    .slice(0, limit);
}

/**
 * 露出用の話者ラベル (§6 個人マスク)。 persona は表示名、 ext は不可逆ペルソナ名 (論者#xxxx)。
 * 公開 ID (speaker_id) を end user に素通ししない。
 */
function speakerLabel(speakerId: string | null): string {
  if (!speakerId) return "参加者";
  if (speakerId.startsWith("persona:")) return speakerId.slice("persona:".length);
  if (speakerId.startsWith("ext:")) return maskedPersonaLabel(speakerId);
  return "人間";
}
