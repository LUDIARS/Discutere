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
  },
  {
    id: "0003_phase3_translation_bridge",
    sql: [
      `CREATE TABLE IF NOT EXISTS translation_proposals (
        id TEXT PRIMARY KEY,
        workspace_id TEXT NOT NULL,
        utterance_id TEXT NOT NULL,
        proposal_type TEXT NOT NULL,
        target_id TEXT,
        target_name TEXT,
        confidence REAL NOT NULL,
        pending_review INTEGER NOT NULL DEFAULT 1,
        status TEXT NOT NULL DEFAULT 'pending',
        payload_json TEXT NOT NULL,
        created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL
      )`,
      `CREATE INDEX IF NOT EXISTS idx_translation_pending ON translation_proposals(workspace_id, pending_review, created_at)`,
      `ALTER TABLE affects ADD COLUMN vocabulary_status TEXT`
    ]
  },
  {
    id: "0004_phase4_gap_detection",
    sql: [
      `ALTER TABLE mechanics ADD COLUMN intended_affect TEXT`,
      `ALTER TABLE affects ADD COLUMN valence TEXT`,
      `ALTER TABLE design_gaps ADD COLUMN gap_in TEXT`,
      `ALTER TABLE design_gaps ADD COLUMN expected_affect TEXT`,
      `ALTER TABLE design_gaps ADD COLUMN observed_affect TEXT`,
      `ALTER TABLE design_gaps ADD COLUMN evidence_json TEXT`,
      `CREATE UNIQUE INDEX IF NOT EXISTS idx_gap_dedup_key ON design_gaps(workspace_id, gap_in, expected_affect, observed_affect)`
    ]
  },
  {
    id: "0005_phase5_hypothesis_lifecycle",
    sql: [
      `ALTER TABLE hypotheses ADD COLUMN stale_flagged INTEGER NOT NULL DEFAULT 0`,
      `CREATE TABLE IF NOT EXISTS hypothesis_validations (
        id TEXT PRIMARY KEY,
        workspace_id TEXT NOT NULL,
        session_id TEXT NOT NULL,
        hypothesis_id TEXT NOT NULL,
        mode TEXT NOT NULL,
        created_at INTEGER NOT NULL
      )`,
      `CREATE INDEX IF NOT EXISTS idx_hv_session ON hypothesis_validations(workspace_id, session_id, created_at)`
    ]
  },
  {
    // KG 共有同期 (spec/core/KG-SYNC.md): 学習知識テーブルに「共有フラグ」を付与。
    // master が shared=1 を立てた行だけが follower への配布対象になる。
    // 注意: CREATE INDEX は ALTER ADD COLUMN の後に置く (既存 DB で no such column 回避)。
    id: "0006_kg_sharing",
    sql: [
      `ALTER TABLE games ADD COLUMN shared INTEGER NOT NULL DEFAULT 0`,
      `ALTER TABLE mechanics ADD COLUMN shared INTEGER NOT NULL DEFAULT 0`,
      `ALTER TABLE aesthetics ADD COLUMN shared INTEGER NOT NULL DEFAULT 0`,
      `ALTER TABLE affects ADD COLUMN shared INTEGER NOT NULL DEFAULT 0`,
      `CREATE INDEX IF NOT EXISTS idx_games_shared ON games(shared, updated_at)`,
      `CREATE INDEX IF NOT EXISTS idx_mechanics_shared ON mechanics(shared, updated_at)`,
      `CREATE INDEX IF NOT EXISTS idx_aesthetics_shared ON aesthetics(shared, updated_at)`,
      `CREATE INDEX IF NOT EXISTS idx_affects_shared ON affects(shared, updated_at)`
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
