/**
 * 弁証法 4 表 (flow_issue / flow_position / flow_tension / flow_synthesis) の永続 (respec 06 §1)。
 *
 * ドメイン型 + CRUD のみ (LLM なし・判断なし)。状態遷移の判断は driver / synthesis / tension が
 * 持ち、ここは書き込み・読み出しの単純な境界に徹する (SRP)。
 */

import { randomUUID } from "node:crypto";
import { getFlowDb } from "../db/connection.js";

// ── ドメイン型 ────────────────────────────────────────────────────────────────

/** 根拠の状態 (dialectic.md §1)。 */
export type GroundState = "unchallenged" | "challenged" | "defended" | "conceded";

export interface Ground {
  id: string;
  text: string;
  state: GroundState;
}

export interface IssueRecord {
  id: string;
  sessionId: string;
  title: string;
  ordinal: number;
  /** llm = [0] の LLM 分解 / paper-gate = 09 の前倒し承認済み論点。 */
  source: "llm" | "paper-gate";
  status: "open" | "concluded";
}

export interface PositionRecord {
  id: string;
  issueId: string;
  personaId: string;
  stance: "pro" | "con";
  claim: string;
  grounds: Ground[];
  values: string[];
}

/** 対立の型 (dialectic.md §2)。 */
export type TensionType = "facts" | "values" | "means" | "tradeoff";

export type TensionStatus =
  | "open"
  | "resolved"
  | "synthesized"
  | "compromised"
  | "agreed_disagree"
  | "unresolved_fact";

export interface TensionRecord {
  id: string;
  issueId: string;
  positionA: string;
  positionB: string;
  type: TensionType;
  status: TensionStatus;
  resolutionNote: string | null;
}

/** 批准 1 件 (両陣営ぶんで 2 要素)。 */
export interface RatificationEntry {
  positionId: string;
  verdict: "accept" | "reject";
  preservedIds: string[];
  missing: string[];
  reason: string;
}

export type SynthesisStatus = "ratified" | "rejected" | "revising";

export interface SynthesisRecord {
  id: string;
  tensionId: string;
  preserves: string[];
  negates: string[];
  /** 導入した新しい枠。null/空 = 折衷 (elevation ゲート)。 */
  elevates: string | null;
  kind: "aufheben" | "compromise";
  /** 露出用の統合文。 */
  text: string;
  ratification: RatificationEntry[];
  status: SynthesisStatus;
  /** 修正ループ回数 (最大 2)。 */
  revision: number;
}

// ── CRUD ─────────────────────────────────────────────────────────────────────

export function insertIssue(issue: Omit<IssueRecord, "id"> & { id?: string }): IssueRecord {
  const id = issue.id ?? randomUUID();
  getFlowDb()
    .prepare(
      `INSERT INTO flow_issue (id, session_id, title, ordinal, source, status, created_at)
       VALUES (?, ?, ?, ?, ?, ?, ?)`
    )
    .run(id, issue.sessionId, issue.title, issue.ordinal, issue.source, issue.status, Date.now());
  return { ...issue, id };
}

export function updateIssueStatus(issueId: string, status: IssueRecord["status"]): void {
  getFlowDb().prepare(`UPDATE flow_issue SET status = ? WHERE id = ?`).run(status, issueId);
}

export function listIssues(sessionId: string): IssueRecord[] {
  const rows = getFlowDb()
    .prepare(
      `SELECT id, session_id, title, ordinal, source, status FROM flow_issue
        WHERE session_id = ? ORDER BY ordinal`
    )
    .all(sessionId) as Array<{
    id: string;
    session_id: string;
    title: string;
    ordinal: number;
    source: string;
    status: string;
  }>;
  return rows.map((r) => ({
    id: r.id,
    sessionId: r.session_id,
    title: r.title,
    ordinal: r.ordinal,
    source: r.source === "paper-gate" ? "paper-gate" : "llm",
    status: r.status === "concluded" ? "concluded" : "open",
  }));
}

export function insertPosition(position: Omit<PositionRecord, "id"> & { id?: string }): PositionRecord {
  const id = position.id ?? randomUUID();
  getFlowDb()
    .prepare(
      `INSERT INTO flow_position (id, issue_id, persona_id, stance, claim, grounds_json, values_json, created_at)
       VALUES (?, ?, ?, ?, ?, ?, ?, ?)`
    )
    .run(
      id,
      position.issueId,
      position.personaId,
      position.stance,
      position.claim,
      JSON.stringify(position.grounds),
      JSON.stringify(position.values),
      Date.now()
    );
  return { ...position, id };
}

/** 根拠の状態遷移を永続に反映する (グラフ更新のたびに write-through)。 */
export function updatePositionGrounds(positionId: string, grounds: readonly Ground[]): void {
  getFlowDb()
    .prepare(`UPDATE flow_position SET grounds_json = ? WHERE id = ?`)
    .run(JSON.stringify(grounds), positionId);
}

function parseJson<T>(s: string, fallback: T): T {
  try {
    return JSON.parse(s) as T;
  } catch {
    return fallback;
  }
}

export function listPositions(issueId: string): PositionRecord[] {
  const rows = getFlowDb()
    .prepare(
      `SELECT id, issue_id, persona_id, stance, claim, grounds_json, values_json
         FROM flow_position WHERE issue_id = ? ORDER BY created_at, id`
    )
    .all(issueId) as Array<{
    id: string;
    issue_id: string;
    persona_id: string;
    stance: string;
    claim: string;
    grounds_json: string;
    values_json: string;
  }>;
  return rows.map((r) => ({
    id: r.id,
    issueId: r.issue_id,
    personaId: r.persona_id,
    stance: r.stance === "con" ? "con" : "pro",
    claim: r.claim,
    grounds: parseJson<Ground[]>(r.grounds_json, []),
    values: parseJson<string[]>(r.values_json, []),
  }));
}

export function insertTension(tension: Omit<TensionRecord, "id"> & { id?: string }): TensionRecord {
  const id = tension.id ?? randomUUID();
  getFlowDb()
    .prepare(
      `INSERT INTO flow_tension (id, issue_id, position_a, position_b, type, status, resolution_note, created_at)
       VALUES (?, ?, ?, ?, ?, ?, ?, ?)`
    )
    .run(
      id,
      tension.issueId,
      tension.positionA,
      tension.positionB,
      tension.type,
      tension.status,
      tension.resolutionNote,
      Date.now()
    );
  return { ...tension, id };
}

export function updateTensionStatus(
  tensionId: string,
  status: TensionStatus,
  resolutionNote?: string | null
): void {
  if (resolutionNote === undefined) {
    getFlowDb().prepare(`UPDATE flow_tension SET status = ? WHERE id = ?`).run(status, tensionId);
  } else {
    getFlowDb()
      .prepare(`UPDATE flow_tension SET status = ?, resolution_note = ? WHERE id = ?`)
      .run(status, resolutionNote, tensionId);
  }
}

export function listTensions(issueId: string): TensionRecord[] {
  const rows = getFlowDb()
    .prepare(
      `SELECT id, issue_id, position_a, position_b, type, status, resolution_note
         FROM flow_tension WHERE issue_id = ? ORDER BY created_at, id`
    )
    .all(issueId) as Array<{
    id: string;
    issue_id: string;
    position_a: string;
    position_b: string;
    type: string;
    status: string;
    resolution_note: string | null;
  }>;
  return rows.map((r) => ({
    id: r.id,
    issueId: r.issue_id,
    positionA: r.position_a,
    positionB: r.position_b,
    type: (["facts", "values", "means", "tradeoff"].includes(r.type) ? r.type : "values") as TensionType,
    status: ([
      "open",
      "resolved",
      "synthesized",
      "compromised",
      "agreed_disagree",
      "unresolved_fact",
    ].includes(r.status)
      ? r.status
      : "open") as TensionStatus,
    resolutionNote: r.resolution_note,
  }));
}

export function insertSynthesis(s: Omit<SynthesisRecord, "id"> & { id?: string }): SynthesisRecord {
  const id = s.id ?? randomUUID();
  getFlowDb()
    .prepare(
      `INSERT INTO flow_synthesis
         (id, tension_id, preserves_json, negates_json, elevates, kind, text, ratification_json, status, revision, created_at)
       VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`
    )
    .run(
      id,
      s.tensionId,
      JSON.stringify(s.preserves),
      JSON.stringify(s.negates),
      s.elevates,
      s.kind,
      s.text,
      JSON.stringify(s.ratification),
      s.status,
      s.revision,
      Date.now()
    );
  return { ...s, id };
}

/** 修正ループの進行 (内容 + 批准 + 状態 + revision) を同一行に上書きする。 */
export function updateSynthesis(
  id: string,
  patch: Pick<SynthesisRecord, "preserves" | "negates" | "elevates" | "kind" | "text" | "ratification" | "status" | "revision">
): void {
  getFlowDb()
    .prepare(
      `UPDATE flow_synthesis
          SET preserves_json = ?, negates_json = ?, elevates = ?, kind = ?, text = ?,
              ratification_json = ?, status = ?, revision = ?
        WHERE id = ?`
    )
    .run(
      JSON.stringify(patch.preserves),
      JSON.stringify(patch.negates),
      patch.elevates,
      patch.kind,
      patch.text,
      JSON.stringify(patch.ratification),
      patch.status,
      patch.revision,
      id
    );
}

export function listSyntheses(tensionId: string): SynthesisRecord[] {
  const rows = getFlowDb()
    .prepare(
      `SELECT id, tension_id, preserves_json, negates_json, elevates, kind, text, ratification_json, status, revision
         FROM flow_synthesis WHERE tension_id = ? ORDER BY created_at, id`
    )
    .all(tensionId) as Array<{
    id: string;
    tension_id: string;
    preserves_json: string;
    negates_json: string;
    elevates: string | null;
    kind: string;
    text: string;
    ratification_json: string;
    status: string;
    revision: number;
  }>;
  return rows.map((r) => ({
    id: r.id,
    tensionId: r.tension_id,
    preserves: parseJson<string[]>(r.preserves_json, []),
    negates: parseJson<string[]>(r.negates_json, []),
    elevates: r.elevates,
    kind: r.kind === "aufheben" ? "aufheben" : "compromise",
    text: r.text,
    ratification: parseJson<RatificationEntry[]>(r.ratification_json, []),
    status: (["ratified", "rejected", "revising"].includes(r.status) ? r.status : "rejected") as SynthesisStatus,
    revision: r.revision,
  }));
}
