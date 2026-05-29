import type Database from "better-sqlite3";

import type {
  PersonaAssignmentRow,
  PersonaFeedbackRow,
  PersonaRow,
  PersonaSeed,
} from "../types.js";

function nowSec(): number {
  return Math.floor(Date.now() / 1000);
}

export class PersonasRepo {
  constructor(private readonly db: Database.Database) {}

  // ─── persona CRUD ──────────────────────────────

  insertOrIgnore(seed: PersonaSeed): void {
    const now = nowSec();
    this.db
      .prepare(
        `INSERT OR IGNORE INTO personas
          (id, name, display_name, description, traits, speech_style, skill_template, learned_notes, created_at, updated_at)
         VALUES (?, ?, ?, ?, ?, ?, '', '[]', ?, ?)`
      )
      .run(
        seed.id,
        seed.name,
        seed.display_name,
        seed.description,
        JSON.stringify(seed.traits),
        seed.speech_style,
        now,
        now
      );
  }

  bulkSeed(seeds: PersonaSeed[]): void {
    const insert = this.db.transaction((rows: PersonaSeed[]) => {
      for (const row of rows) this.insertOrIgnore(row);
    });
    insert(seeds);
  }

  get(id: string): PersonaRow | undefined {
    return this.db.prepare("SELECT * FROM personas WHERE id = ?").get(id) as
      | PersonaRow
      | undefined;
  }

  list(): PersonaRow[] {
    return this.db
      .prepare("SELECT * FROM personas ORDER BY id")
      .all() as PersonaRow[];
  }

  // ─── assignment ────────────────────────────────

  assign(personaId: string, sessionId: string): PersonaAssignmentRow {
    const now = nowSec();
    const info = this.db
      .prepare(
        "INSERT INTO persona_assignments (persona_id, session_id, assigned_at) VALUES (?, ?, ?)"
      )
      .run(personaId, sessionId, now);
    return this.db
      .prepare("SELECT * FROM persona_assignments WHERE id = ?")
      .get(info.lastInsertRowid) as PersonaAssignmentRow;
  }

  release(assignmentId: number): void {
    this.db
      .prepare("UPDATE persona_assignments SET released_at = ? WHERE id = ?")
      .run(nowSec(), assignmentId);
  }

  /** session で active な assignment (released_at IS NULL) を返す */
  activeAssignmentsForSession(sessionId: string): PersonaAssignmentRow[] {
    return this.db
      .prepare(
        "SELECT * FROM persona_assignments WHERE session_id = ? AND released_at IS NULL ORDER BY assigned_at"
      )
      .all(sessionId) as PersonaAssignmentRow[];
  }

  // ─── feedback / learned_notes ──────────────────

  addFeedback(input: {
    persona_id: string;
    session_id?: string | null;
    kind: PersonaFeedbackRow["kind"];
    delta: string;
    detail?: string;
  }): PersonaFeedbackRow {
    const info = this.db
      .prepare(
        "INSERT INTO persona_feedback (persona_id, session_id, ts, kind, delta, detail) VALUES (?, ?, ?, ?, ?, ?)"
      )
      .run(
        input.persona_id,
        input.session_id ?? null,
        nowSec(),
        input.kind,
        input.delta,
        input.detail ?? null
      );
    return this.db
      .prepare("SELECT * FROM persona_feedback WHERE id = ?")
      .get(info.lastInsertRowid) as PersonaFeedbackRow;
  }

  feedbackFor(personaId: string, limit = 50): PersonaFeedbackRow[] {
    return this.db
      .prepare(
        "SELECT * FROM persona_feedback WHERE persona_id = ? ORDER BY ts DESC LIMIT ?"
      )
      .all(personaId, limit) as PersonaFeedbackRow[];
  }

  /** learned_notes に 1 行追加 (JSON 配列に append) */
  appendLearnedNote(personaId: string, note: string): void {
    const row = this.get(personaId);
    if (!row) throw new Error(`persona not found: ${personaId}`);
    const arr = safeJsonArray(row.learned_notes);
    arr.push({ note, ts: nowSec() });
    this.db
      .prepare("UPDATE personas SET learned_notes = ?, updated_at = ? WHERE id = ?")
      .run(JSON.stringify(arr), nowSec(), personaId);
  }
}

function safeJsonArray(raw: string): Array<{ note: string; ts: number }> {
  try {
    const parsed = JSON.parse(raw);
    return Array.isArray(parsed) ? parsed : [];
  } catch {
    return [];
  }
}
