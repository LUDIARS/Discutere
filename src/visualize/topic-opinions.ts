import type Database from "better-sqlite3";

export interface TopicOpinionEntry {
  id: string;
  statement: string;
  status: string | null;
  integrated: boolean;
  updatedAt: number;
}

export interface TopicEntry {
  id: string;
  title: string;
  description: string | null;
  status: string | null;
  gapIn: string | null;
  expectedAffect: string | null;
  observedAffect: string | null;
  opinionCount: number;
  utteranceCount: number;
  size: number;
  updatedAt: number;
  opinions: TopicOpinionEntry[];
}

export interface TopicOpinionSnapshot {
  workspaceId: string;
  generatedAt: number;
  topics: TopicEntry[];
  totals: {
    topics: number;
    opinions: number;
    utterances: number;
  };
}

export interface TopicOpinionOptions {
  limit?: number;
  opinionsPerTopic?: number;
}

export function buildTopicOpinionSnapshot(
  db: Database.Database,
  workspaceId: string,
  options: TopicOpinionOptions = {}
): TopicOpinionSnapshot {
  const limit = clampLimit(options.limit ?? 40, 1, 200);
  const opinionsPerTopic = clampLimit(options.opinionsPerTopic ?? 8, 1, 50);

  const topicRows = db
    .prepare(
      `SELECT id, title, description, status, gap_in, expected_affect, observed_affect, updated_at
         FROM design_gaps
        WHERE workspace_id = ?
        ORDER BY updated_at DESC
        LIMIT ?`
    )
    .all(workspaceId, limit) as Array<{
    id: string;
    title: string;
    description: string | null;
    status: string | null;
    gap_in: string | null;
    expected_affect: string | null;
    observed_affect: string | null;
    updated_at: number;
  }>;

  const opinionStmt = db.prepare(
    `SELECT id, statement, status, integrated, updated_at
       FROM hypotheses
      WHERE workspace_id = ? AND design_gap_id = ?
      ORDER BY updated_at DESC
      LIMIT ?`
  );
  const opinionCountStmt = db.prepare(
    "SELECT COUNT(*) AS c FROM hypotheses WHERE workspace_id = ? AND design_gap_id = ?"
  );
  const utteranceCountStmt = db.prepare(
    `SELECT COUNT(*) AS c
       FROM utterances u
       JOIN sessions s ON s.id = u.session_id AND s.workspace_id = u.workspace_id
      WHERE u.workspace_id = ?
        AND (s.scene = ? OR s.title = ?)`
  );

  const topics = topicRows.map((row) => {
    const opinions = (
      opinionStmt.all(workspaceId, row.id, opinionsPerTopic) as Array<{
        id: string;
        statement: string;
        status: string | null;
        integrated: number;
        updated_at: number;
      }>
    ).map((opinion) => ({
      id: opinion.id,
      statement: opinion.statement,
      status: opinion.status,
      integrated: opinion.integrated === 1,
      updatedAt: opinion.updated_at,
    }));
    const opinionCount = (opinionCountStmt.get(workspaceId, row.id) as { c: number }).c;
    const utteranceCount = (
      utteranceCountStmt.get(workspaceId, `gap:${row.id}`, `discussion-of-gap:${row.id}`) as {
        c: number;
      }
    ).c;
    return {
      id: row.id,
      title: row.title,
      description: row.description,
      status: row.status,
      gapIn: row.gap_in,
      expectedAffect: row.expected_affect,
      observedAffect: row.observed_affect,
      opinionCount,
      utteranceCount,
      size: opinionCount * 3 + utteranceCount,
      updatedAt: row.updated_at,
      opinions,
    };
  });

  topics.sort((a, b) => b.size - a.size || b.updatedAt - a.updatedAt);

  return {
    workspaceId,
    generatedAt: Date.now(),
    topics,
    totals: {
      topics: topics.length,
      opinions: topics.reduce((sum, topic) => sum + topic.opinionCount, 0),
      utterances: topics.reduce((sum, topic) => sum + topic.utteranceCount, 0),
    },
  };
}

function clampLimit(value: number, min: number, max: number): number {
  if (!Number.isFinite(value)) return min;
  return Math.min(max, Math.max(min, Math.floor(value)));
}
