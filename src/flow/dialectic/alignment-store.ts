/**
 * すり合わせ (flow_alignment) の永続 (dialectic.md §4.5)。
 *
 * ドメイン型 + CRUD のみ (LLM なし・判断なし)。生成と検証は alignment.ts が持つ (SRP)。
 */

import { randomUUID } from "node:crypto";
import { getFlowDb } from "../db/connection.js";
import type { AttackPoint } from "./premise-rules.js";

/** まだ決まっていない点 1 件。 */
export interface OpenPoint {
  /** 未決の根拠 id (主張の限定そのものが未決なら null)。 */
  groundId: string | null;
  /** 決まっていないのはデータ・論拠・限定のどれか。 */
  point: AttackPoint;
  /** 何が分かれば決まるか。 */
  need: string;
}

export interface AlignmentRecord {
  id: string;
  tensionId: string;
  /** どんな体験をつくりたいか。 */
  experience: string;
  /** どんな施策 (アイデア) を試したいか。 */
  measure: string;
  /** 他のゲームではどうしているか / どうなっているか。 */
  references: string;
  /** 両陣営が合意した根拠 id。 */
  agreed: string[];
  open: OpenPoint[];
  /** 露出用のまとめ文。 */
  text: string;
}

export function insertAlignment(a: Omit<AlignmentRecord, "id"> & { id?: string }): AlignmentRecord {
  const id = a.id ?? randomUUID();
  getFlowDb()
    .prepare(
      `INSERT INTO flow_alignment
         (id, tension_id, experience, measure, reference_cases, agreed_json, open_json, text, created_at)
       VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)`
    )
    .run(
      id,
      a.tensionId,
      a.experience,
      a.measure,
      a.references,
      JSON.stringify(a.agreed),
      JSON.stringify(a.open),
      a.text,
      Date.now()
    );
  return { ...a, id };
}

function parseJson<T>(s: string, fallback: T): T {
  try {
    return JSON.parse(s) as T;
  } catch {
    return fallback;
  }
}

export function listAlignments(tensionId: string): AlignmentRecord[] {
  const rows = getFlowDb()
    .prepare(
      `SELECT id, tension_id, experience, measure, reference_cases, agreed_json, open_json, text
         FROM flow_alignment WHERE tension_id = ? ORDER BY created_at, id`
    )
    .all(tensionId) as Array<{
    id: string;
    tension_id: string;
    experience: string;
    measure: string;
    reference_cases: string;
    agreed_json: string;
    open_json: string;
    text: string;
  }>;
  return rows.map((r) => ({
    id: r.id,
    tensionId: r.tension_id,
    experience: r.experience,
    measure: r.measure,
    references: r.reference_cases,
    agreed: parseJson<string[]>(r.agreed_json, []),
    open: parseJson<OpenPoint[]>(r.open_json, []),
    text: r.text,
  }));
}
