import type Database from "better-sqlite3";

// @implements SPEC-PERSONA-BRIDGE-UTTERANCE-EXPORT
export interface ExportedUtterance {
  text: string;
  createdAt: string;
}

/** DB ordering metadata kept out of the persona bridge response body. */
export interface ExportableUtteranceRecord extends ExportedUtterance {
  id: string;
}

export interface UtteranceCursor {
  postedAt: number;
  id: string;
}

const CURSOR_PATTERN = /^[A-Za-z0-9_-]+$/;
const MAX_CURSOR_LENGTH = 512;
const MAX_UTTERANCE_ID_LENGTH = 256;

export function encodeUtteranceCursor(cursor: UtteranceCursor): string {
  return Buffer.from(JSON.stringify([cursor.postedAt, cursor.id]), "utf8").toString("base64url");
}

export function decodeUtteranceCursor(value: string): UtteranceCursor {
  if (!value || value.length > MAX_CURSOR_LENGTH || !CURSOR_PATTERN.test(value)) {
    throw new TypeError("cursor is invalid");
  }
  try {
    const parsed = JSON.parse(Buffer.from(value, "base64url").toString("utf8")) as unknown;
    if (
      !Array.isArray(parsed)
      || parsed.length !== 2
      || typeof parsed[0] !== "number"
      || !Number.isSafeInteger(parsed[0])
      || parsed[0] < 0
      || typeof parsed[1] !== "string"
      || parsed[1].length === 0
      || parsed[1].length > MAX_UTTERANCE_ID_LENGTH
    ) {
      throw new TypeError("invalid cursor payload");
    }
    return { postedAt: parsed[0], id: parsed[1] };
  } catch {
    throw new TypeError("cursor is invalid");
  }
}

export function nextUtteranceCursor(
  utterances: readonly ExportableUtteranceRecord[]
): string | null {
  const last = utterances.at(-1);
  if (!last) return null;
  const postedAt = Date.parse(last.createdAt);
  if (!Number.isSafeInteger(postedAt) || postedAt < 0) {
    throw new TypeError("utterance createdAt is invalid");
  }
  return encodeUtteranceCursor({ postedAt, id: last.id });
}

export function readHumanUtterances(
  db: Database.Database,
  {
    authorId,
    after,
    limit,
  }: {
    authorId: string;
    after: UtteranceCursor;
    limit: number;
  }
): ExportableUtteranceRecord[] {
  const rows = db.prepare(
    `SELECT u.id, u.raw_content AS text, u.posted_at AS postedAt
       FROM utterances u
       JOIN sessions s
         ON s.id = u.session_id
        AND s.workspace_id = u.workspace_id
      WHERE u.speaker_id = ?
        AND s.title LIKE 'discord-session:%'
        AND s.scene LIKE 'discord:%'
        AND (u.posted_at > ? OR (u.posted_at = ? AND u.id > ?))
      ORDER BY u.posted_at ASC, u.id ASC
      LIMIT ?`
  ).all(authorId, after.postedAt, after.postedAt, after.id, limit) as Array<{
    id: string;
    text: string;
    postedAt: number;
  }>;
  return rows.map((row) => ({
    id: row.id,
    text: row.text,
    createdAt: new Date(row.postedAt).toISOString(),
  }));
}
