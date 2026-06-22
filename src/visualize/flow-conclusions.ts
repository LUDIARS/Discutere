/**
 * 新フロー (src/flow/) の議論結論を学習ビューの「結論」タブに載せる (新フロー結論統合)。
 *
 * 旧 auto-discussion は design_gaps → sessions → utterances に書くが、
 * 新フロー (T1-T7) は discussion_paper / flow_utterance / flow_conclusion に書くため、
 * 旧 conclusions.ts (design_gaps 専用) では新フロー議論が一切表示されなかった。
 *
 * ここは flow_* テーブルを読み、conclusions.ts と同じ ConclusionSummary / ConclusionDetail
 * 形状で返す (フロントは無改修で両者を並べられる)。一意キーは "flow:<sessionId>"。
 */

import Database from "better-sqlite3";

import {
  composeDisplayName,
} from "../flow/persona-display.js";
import type { FlowRole, FlowStance } from "../flow/personas.js";
import type { ConclusionSummary, ConclusionDetail, ConclusionPaper } from "./conclusions.js";

/** flow_conclusion.summary の収束プレフィックス (flow/conclusion.ts と一致)。 */
const CONVERGE_PREFIX = "【収束】";

/** "flow:<sessionId>" 形式の合成 id の接頭辞。 */
export const FLOW_CONCLUSION_PREFIX = "flow:";

export function isFlowConclusionId(gapId: string): boolean {
  return gapId.startsWith(FLOW_CONCLUSION_PREFIX);
}

export function flowSessionIdFromGapId(gapId: string): string {
  return gapId.slice(FLOW_CONCLUSION_PREFIX.length);
}

interface ConclusionRow {
  session_id: string;
  paper_id: string;
  summary: string;
  aufhebung_json: string;
  top_utterance_ids_json: string;
  concluded: number;
  created_at: number;
  theme: string | null;
  // 詳細取得時のみ JOIN で引く (一覧では未選択 = undefined)。
  tags_json?: string | null;
  mechanics_json?: string | null;
  supplement?: string | null;
}

/** discussion_paper 由来の生 mechanics JSON 1 要素を表示用に正規化する。 */
function normalizeMechanic(raw: unknown): { name: string; description: string; intendedAffect?: string } | null {
  if (!raw || typeof raw !== "object") return null;
  const m = raw as Record<string, unknown>;
  const name = typeof m.name === "string" ? m.name : "";
  if (!name) return null;
  return {
    name,
    description: typeof m.description === "string" ? m.description : "",
    intendedAffect: typeof m.intended_affect === "string" ? m.intended_affect : undefined,
  };
}

function parseMechanics(raw: string | null | undefined): Array<{ name: string; description: string; intendedAffect?: string }> {
  if (!raw) return [];
  try {
    const parsed = JSON.parse(raw);
    if (!Array.isArray(parsed)) return [];
    return parsed.map(normalizeMechanic).filter((m): m is { name: string; description: string; intendedAffect?: string } => m !== null);
  } catch {
    return [];
  }
}

/**
 * discussion_paper の列からディスカッションペーパーを組み立てる。
 * paper が JOIN できなかった (paper_id 不一致 = theme null) 場合は null を返す。
 */
function buildPaper(r: ConclusionRow): ConclusionPaper | null {
  if (r.theme === null && !r.tags_json && !r.mechanics_json && !r.supplement) return null;
  return {
    theme: r.theme ?? "",
    tags: parseJsonArray(r.tags_json ?? "[]"),
    supplement: (r.supplement ?? "").trim(),
    mechanics: parseMechanics(r.mechanics_json),
  };
}

interface FlowUtteranceRow {
  id: string;
  persona_name: string;
  role: string;
  stance: string;
  possession_name: string | null;
  text: string;
  is_error: number;
}

/** 【収束】プレフィックスを外し、空 / 「(結論なし)」は null にする。 */
function cleanSummary(summary: string, concluded: number): string | null {
  if (!concluded) return null;
  const body = summary.startsWith(CONVERGE_PREFIX)
    ? summary.slice(CONVERGE_PREFIX.length).trim()
    : summary.trim();
  if (!body || body === "(結論なし)") return null;
  return body;
}

function parseJsonArray(raw: string): string[] {
  try {
    const parsed = JSON.parse(raw);
    return Array.isArray(parsed) ? parsed.filter((x): x is string => typeof x === "string") : [];
  } catch {
    return [];
  }
}

/** 露出用の話者ラベル「名前 (ロール/憑依)」。 flow_utterance は内部ペルソナのみ (出所 null)。 */
function speakerLabel(u: { persona_name: string; stance: string; role: string; possession_name: string | null }): string {
  return composeDisplayName({
    name: u.persona_name,
    stance: u.stance as FlowStance,
    role: u.role as FlowRole,
    possessionName: u.possession_name,
  });
}

function utteranceCount(db: Database.Database, sessionId: string): number {
  const row = db
    .prepare("SELECT COUNT(*) AS n FROM flow_utterance WHERE session_id = ?")
    .get(sessionId) as { n: number };
  return row.n;
}

function summaryToSummaryRow(r: ConclusionRow, db: Database.Database): ConclusionSummary {
  return {
    gapId: `${FLOW_CONCLUSION_PREFIX}${r.session_id}`,
    kind: "flow",
    sessionId: r.session_id,
    title: r.theme ?? "(無題の議論)",
    conclusion: cleanSummary(r.summary, r.concluded),
    utteranceCount: utteranceCount(db, r.session_id),
    aufhebungCount: parseJsonArray(r.aufhebung_json).length,
    updatedAt: r.created_at,
  };
}

/** 新フロー結論を新しい順に一覧する (flow_conclusion 全件)。 */
export function listFlowConclusions(db: Database.Database, limit = 100): ConclusionSummary[] {
  const rows = db
    .prepare(
      `SELECT c.session_id, c.paper_id, c.summary, c.aufhebung_json, c.top_utterance_ids_json,
              c.concluded, c.created_at, p.theme AS theme
         FROM flow_conclusion c
         LEFT JOIN discussion_paper p ON p.id = c.paper_id
        ORDER BY c.created_at DESC
        LIMIT ?`
    )
    .all(limit) as ConclusionRow[];
  return rows.map((r) => summaryToSummaryRow(r, db));
}

/** 1 件の新フロー結論の裏にある論述データ (議論ログ / 止揚 / 高評価意見)。 無ければ null。 */
export function getFlowConclusionDetail(db: Database.Database, sessionId: string): ConclusionDetail | null {
  const row = db
    .prepare(
      `SELECT c.session_id, c.paper_id, c.summary, c.aufhebung_json, c.top_utterance_ids_json,
              c.concluded, c.created_at, p.theme AS theme,
              p.tags_json AS tags_json, p.mechanics_json AS mechanics_json, p.supplement AS supplement
         FROM flow_conclusion c
         LEFT JOIN discussion_paper p ON p.id = c.paper_id
        WHERE c.session_id = ?`
    )
    .get(sessionId) as ConclusionRow | undefined;
  if (!row) return null;

  const summary = summaryToSummaryRow(row, db);
  const transcriptRows = db
    .prepare(
      `SELECT id, persona_name, role, stance, possession_name, text, is_error
         FROM flow_utterance
        WHERE session_id = ?
        ORDER BY created_at ASC`
    )
    .all(sessionId) as FlowUtteranceRow[];

  // 高評価意見 = 投票で勝った発話 (top_utterance_ids_json)。 スコアは vote 表の得票数。
  const topIds = new Set(parseJsonArray(row.top_utterance_ids_json));
  const topOpinions = transcriptRows
    .filter((u) => topIds.has(u.id))
    .map((u) => ({
      speaker: speakerLabel(u),
      content: u.text,
      score: voteCount(db, u.id),
      source: null,
      sourceUrl: null,
    }))
    .sort((a, b) => b.score - a.score);

  return {
    ...summary,
    paper: buildPaper(row),
    aufhebungen: parseJsonArray(row.aufhebung_json),
    topOpinions,
    transcript: transcriptRows
      .filter((u) => !u.is_error)
      .map((u) => ({
        speaker: speakerLabel(u),
        content: u.text,
        source: null,
        sourceUrl: null,
      })),
  };
}

function voteCount(db: Database.Database, utteranceId: string): number {
  const row = db
    .prepare("SELECT COUNT(*) AS n FROM vote WHERE chosen_utterance_id = ?")
    .get(utteranceId) as { n: number } | undefined;
  return row?.n ?? 0;
}
