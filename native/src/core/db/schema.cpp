#include "core/db/schema.hpp"

#include "base/time.hpp"

namespace di::core::db {

const std::vector<Migration>& core_migrations() {
    static const std::vector<Migration> migrations = {
        {"0001_core_schema",
         {
             R"SQL(CREATE TABLE IF NOT EXISTS _migrations (
        id TEXT PRIMARY KEY,
        applied_at INTEGER NOT NULL
      ))SQL",
             R"SQL(CREATE TABLE IF NOT EXISTS events (
        id TEXT PRIMARY KEY,
        event_type TEXT NOT NULL,
        payload_json TEXT NOT NULL,
        created_at INTEGER NOT NULL
      ))SQL",
             R"SQL(CREATE TABLE IF NOT EXISTS people (id TEXT PRIMARY KEY,workspace_id TEXT NOT NULL,name TEXT NOT NULL,role TEXT,created_at INTEGER NOT NULL,updated_at INTEGER NOT NULL))SQL",
             R"SQL(CREATE TABLE IF NOT EXISTS games (id TEXT PRIMARY KEY,workspace_id TEXT NOT NULL,title TEXT NOT NULL,genre TEXT,created_at INTEGER NOT NULL,updated_at INTEGER NOT NULL))SQL",
             R"SQL(CREATE TABLE IF NOT EXISTS mechanics (id TEXT PRIMARY KEY,workspace_id TEXT NOT NULL,game_id TEXT,name TEXT NOT NULL,description TEXT,created_at INTEGER NOT NULL,updated_at INTEGER NOT NULL))SQL",
             R"SQL(CREATE TABLE IF NOT EXISTS aesthetics (id TEXT PRIMARY KEY,workspace_id TEXT NOT NULL,game_id TEXT,name TEXT NOT NULL,description TEXT,created_at INTEGER NOT NULL,updated_at INTEGER NOT NULL))SQL",
             R"SQL(CREATE TABLE IF NOT EXISTS affects (id TEXT PRIMARY KEY,workspace_id TEXT NOT NULL,session_id TEXT,subject_id TEXT,mood TEXT NOT NULL,score REAL,created_at INTEGER NOT NULL,updated_at INTEGER NOT NULL))SQL",
             R"SQL(CREATE TABLE IF NOT EXISTS play_contexts (id TEXT PRIMARY KEY,workspace_id TEXT NOT NULL,game_id TEXT,platform TEXT,mode TEXT,created_at INTEGER NOT NULL,updated_at INTEGER NOT NULL))SQL",
             R"SQL(CREATE TABLE IF NOT EXISTS sessions (id TEXT PRIMARY KEY,workspace_id TEXT NOT NULL,title TEXT NOT NULL,started_at INTEGER NOT NULL,ended_at INTEGER,created_at INTEGER NOT NULL,updated_at INTEGER NOT NULL))SQL",
             R"SQL(CREATE TABLE IF NOT EXISTS utterances (id TEXT PRIMARY KEY,workspace_id TEXT NOT NULL,session_id TEXT NOT NULL,speaker_id TEXT,raw_content TEXT NOT NULL,normalized_content TEXT,posted_at INTEGER NOT NULL,created_at INTEGER NOT NULL,updated_at INTEGER NOT NULL))SQL",
             R"SQL(CREATE TABLE IF NOT EXISTS reactions (id TEXT PRIMARY KEY,workspace_id TEXT NOT NULL,utterance_id TEXT NOT NULL,actor_id TEXT,reaction_type TEXT NOT NULL,intensity REAL,created_at INTEGER NOT NULL,updated_at INTEGER NOT NULL))SQL",
             R"SQL(CREATE TABLE IF NOT EXISTS design_gaps (id TEXT PRIMARY KEY,workspace_id TEXT NOT NULL,game_id TEXT,title TEXT NOT NULL,description TEXT,status TEXT,created_at INTEGER NOT NULL,updated_at INTEGER NOT NULL))SQL",
             R"SQL(CREATE TABLE IF NOT EXISTS hypotheses (id TEXT PRIMARY KEY,workspace_id TEXT NOT NULL,design_gap_id TEXT,statement TEXT NOT NULL,status TEXT,integrated INTEGER NOT NULL DEFAULT 0,created_at INTEGER NOT NULL,updated_at INTEGER NOT NULL))SQL",
             R"SQL(CREATE TABLE IF NOT EXISTS embeddings (id TEXT PRIMARY KEY,workspace_id TEXT NOT NULL,node_type TEXT NOT NULL,node_id TEXT NOT NULL,vector_json TEXT NOT NULL,created_at INTEGER NOT NULL))SQL",
             R"SQL(CREATE INDEX IF NOT EXISTS idx_events_created_at ON events(created_at))SQL",
             R"SQL(CREATE INDEX IF NOT EXISTS idx_embeddings_ws_type ON embeddings(workspace_id, node_type))SQL",
             R"SQL(CREATE UNIQUE INDEX IF NOT EXISTS idx_embeddings_node ON embeddings(workspace_id, node_type, node_id))SQL"}},
        {"0002_phase2_message_projection",
         {
             R"SQL(ALTER TABLE sessions ADD COLUMN mode TEXT)SQL",
             R"SQL(ALTER TABLE sessions ADD COLUMN scene TEXT)SQL",
             R"SQL(ALTER TABLE utterances ADD COLUMN responds_to TEXT)SQL",
             R"SQL(ALTER TABLE hypotheses ADD COLUMN validated_by_emotion INTEGER NOT NULL DEFAULT 0)SQL",
             R"SQL(ALTER TABLE hypotheses ADD COLUMN refs_json TEXT)SQL",
             R"SQL(ALTER TABLE mechanics ADD COLUMN intends TEXT)SQL"}},
        {"0003_phase3_translation_bridge",
         {
             R"SQL(CREATE TABLE IF NOT EXISTS translation_proposals (
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
      ))SQL",
             R"SQL(CREATE INDEX IF NOT EXISTS idx_translation_pending ON translation_proposals(workspace_id, pending_review, created_at))SQL",
             R"SQL(ALTER TABLE affects ADD COLUMN vocabulary_status TEXT)SQL"}},
        {"0004_phase4_gap_detection",
         {
             R"SQL(ALTER TABLE mechanics ADD COLUMN intended_affect TEXT)SQL",
             R"SQL(ALTER TABLE affects ADD COLUMN valence TEXT)SQL",
             R"SQL(ALTER TABLE design_gaps ADD COLUMN gap_in TEXT)SQL",
             R"SQL(ALTER TABLE design_gaps ADD COLUMN expected_affect TEXT)SQL",
             R"SQL(ALTER TABLE design_gaps ADD COLUMN observed_affect TEXT)SQL",
             R"SQL(ALTER TABLE design_gaps ADD COLUMN evidence_json TEXT)SQL",
             R"SQL(CREATE UNIQUE INDEX IF NOT EXISTS idx_gap_dedup_key ON design_gaps(workspace_id, gap_in, expected_affect, observed_affect))SQL"}},
        {"0005_phase5_hypothesis_lifecycle",
         {
             R"SQL(ALTER TABLE hypotheses ADD COLUMN stale_flagged INTEGER NOT NULL DEFAULT 0)SQL",
             R"SQL(CREATE TABLE IF NOT EXISTS hypothesis_validations (
        id TEXT PRIMARY KEY,
        workspace_id TEXT NOT NULL,
        session_id TEXT NOT NULL,
        hypothesis_id TEXT NOT NULL,
        mode TEXT NOT NULL,
        created_at INTEGER NOT NULL
      ))SQL",
             R"SQL(CREATE INDEX IF NOT EXISTS idx_hv_session ON hypothesis_validations(workspace_id, session_id, created_at))SQL"}},
        {"0006_kg_sharing",
         {
             R"SQL(ALTER TABLE games ADD COLUMN shared INTEGER NOT NULL DEFAULT 0)SQL",
             R"SQL(ALTER TABLE mechanics ADD COLUMN shared INTEGER NOT NULL DEFAULT 0)SQL",
             R"SQL(ALTER TABLE aesthetics ADD COLUMN shared INTEGER NOT NULL DEFAULT 0)SQL",
             R"SQL(ALTER TABLE affects ADD COLUMN shared INTEGER NOT NULL DEFAULT 0)SQL",
             R"SQL(CREATE INDEX IF NOT EXISTS idx_games_shared ON games(shared, updated_at))SQL",
             R"SQL(CREATE INDEX IF NOT EXISTS idx_mechanics_shared ON mechanics(shared, updated_at))SQL",
             R"SQL(CREATE INDEX IF NOT EXISTS idx_aesthetics_shared ON aesthetics(shared, updated_at))SQL",
             R"SQL(CREATE INDEX IF NOT EXISTS idx_affects_shared ON affects(shared, updated_at))SQL"}},
    };
    return migrations;
}

void apply_migrations(sql::Database& db) {
    std::lock_guard lock(db.mutex());
    db.exec("CREATE TABLE IF NOT EXISTS _migrations (id TEXT PRIMARY KEY, applied_at INTEGER NOT NULL)");
    db.exec("BEGIN");
    try {
        for (const auto& migration : core_migrations()) {
            if (db.get("SELECT 1 FROM _migrations WHERE id = ?", {migration.id})) continue;
            for (const auto& stmt : migration.sql) {
                try {
                    db.exec(stmt);
                } catch (const sql::Error&) {
                    // additive migration no-op when already applied
                }
            }
            db.run("INSERT INTO _migrations (id, applied_at) VALUES (?, ?)",
                   {migration.id, static_cast<double>(time::now_ms())});
        }
        db.exec("COMMIT");
    } catch (...) {
        try {
            db.exec("ROLLBACK");
        } catch (...) {
        }
        throw;
    }
}

} // namespace di::core::db
