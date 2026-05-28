import type Database from "better-sqlite3";

const MIGRATIONS = [
  {
    id: "0001_core_schema",
    sql: [
      `CREATE TABLE IF NOT EXISTS _migrations (
        id TEXT PRIMARY KEY,
        applied_at INTEGER NOT NULL
      )`,
      `CREATE TABLE IF NOT EXISTS events (
        id TEXT PRIMARY KEY,
        event_type TEXT NOT NULL,
        payload_json TEXT NOT NULL,
        created_at INTEGER NOT NULL
      )`,
      `CREATE TABLE IF NOT EXISTS people (id TEXT PRIMARY KEY,workspace_id TEXT NOT NULL,name TEXT NOT NULL,role TEXT,created_at INTEGER NOT NULL,updated_at INTEGER NOT NULL)`,
      `CREATE TABLE IF NOT EXISTS games (id TEXT PRIMARY KEY,workspace_id TEXT NOT NULL,title TEXT NOT NULL,genre TEXT,created_at INTEGER NOT NULL,updated_at INTEGER NOT NULL)`,
      `CREATE TABLE IF NOT EXISTS mechanics (id TEXT PRIMARY KEY,workspace_id TEXT NOT NULL,game_id TEXT,name TEXT NOT NULL,description TEXT,created_at INTEGER NOT NULL,updated_at INTEGER NOT NULL)`,
      `CREATE TABLE IF NOT EXISTS aesthetics (id TEXT PRIMARY KEY,workspace_id TEXT NOT NULL,game_id TEXT,name TEXT NOT NULL,description TEXT,created_at INTEGER NOT NULL,updated_at INTEGER NOT NULL)`,
      `CREATE TABLE IF NOT EXISTS affects (id TEXT PRIMARY KEY,workspace_id TEXT NOT NULL,session_id TEXT,subject_id TEXT,mood TEXT NOT NULL,score REAL,created_at INTEGER NOT NULL,updated_at INTEGER NOT NULL)`,
      `CREATE TABLE IF NOT EXISTS play_contexts (id TEXT PRIMARY KEY,workspace_id TEXT NOT NULL,game_id TEXT,platform TEXT,mode TEXT,created_at INTEGER NOT NULL,updated_at INTEGER NOT NULL)`,
      `CREATE TABLE IF NOT EXISTS sessions (id TEXT PRIMARY KEY,workspace_id TEXT NOT NULL,title TEXT NOT NULL,started_at INTEGER NOT NULL,ended_at INTEGER,created_at INTEGER NOT NULL,updated_at INTEGER NOT NULL)`,
      `CREATE TABLE IF NOT EXISTS utterances (id TEXT PRIMARY KEY,workspace_id TEXT NOT NULL,session_id TEXT NOT NULL,speaker_id TEXT,raw_content TEXT NOT NULL,normalized_content TEXT,posted_at INTEGER NOT NULL,created_at INTEGER NOT NULL,updated_at INTEGER NOT NULL)`,
      `CREATE TABLE IF NOT EXISTS reactions (id TEXT PRIMARY KEY,workspace_id TEXT NOT NULL,utterance_id TEXT NOT NULL,actor_id TEXT,reaction_type TEXT NOT NULL,intensity REAL,created_at INTEGER NOT NULL,updated_at INTEGER NOT NULL)`,
      `CREATE TABLE IF NOT EXISTS design_gaps (id TEXT PRIMARY KEY,workspace_id TEXT NOT NULL,game_id TEXT,title TEXT NOT NULL,description TEXT,status TEXT,created_at INTEGER NOT NULL,updated_at INTEGER NOT NULL)`,
      `CREATE TABLE IF NOT EXISTS hypotheses (id TEXT PRIMARY KEY,workspace_id TEXT NOT NULL,design_gap_id TEXT,statement TEXT NOT NULL,status TEXT,integrated INTEGER NOT NULL DEFAULT 0,created_at INTEGER NOT NULL,updated_at INTEGER NOT NULL)`,
      `CREATE TABLE IF NOT EXISTS embeddings (id TEXT PRIMARY KEY,workspace_id TEXT NOT NULL,node_type TEXT NOT NULL,node_id TEXT NOT NULL,vector_json TEXT NOT NULL,created_at INTEGER NOT NULL)`,
      `CREATE INDEX IF NOT EXISTS idx_events_created_at ON events(created_at)`,
      `CREATE INDEX IF NOT EXISTS idx_embeddings_ws_type ON embeddings(workspace_id, node_type)`,
      `CREATE UNIQUE INDEX IF NOT EXISTS idx_embeddings_node ON embeddings(workspace_id, node_type, node_id)`,
    ],
  },
  {
    id: "0002_phase2_message_projection",
    sql: [
      `ALTER TABLE sessions ADD COLUMN mode TEXT`,
      `ALTER TABLE sessions ADD COLUMN scene TEXT`,
      `ALTER TABLE utterances ADD COLUMN responds_to TEXT`,
      `ALTER TABLE hypotheses ADD COLUMN validated_by_emotion INTEGER NOT NULL DEFAULT 0`,
      `ALTER TABLE hypotheses ADD COLUMN refs_json TEXT`,
      `ALTER TABLE mechanics ADD COLUMN intends TEXT`
    ]
  }
];

export function applyMigrations(db: Database.Database): void {
  db.exec(`CREATE TABLE IF NOT EXISTS _migrations (id TEXT PRIMARY KEY, applied_at INTEGER NOT NULL)`);
  db.exec("BEGIN");
  try {
    for (const migration of MIGRATIONS) {
      const exists = db.prepare("SELECT 1 FROM _migrations WHERE id = ?").get(migration.id) as { 1: number } | undefined;
      if (exists) continue;
      for (const stmt of migration.sql) {
        try { db.exec(stmt); } catch { /* additive migration no-op when already applied */ }
      }
      db.prepare("INSERT INTO _migrations (id, applied_at) VALUES (?, ?)").run(migration.id, Date.now());
    }
    db.exec("COMMIT");
  } catch (e) {
    db.exec("ROLLBACK");
    throw e;
  }
}
