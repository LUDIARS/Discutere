#include "persona_engine/db/migrations.hpp"

#include "base/time.hpp"

namespace di::persona_engine::db {

const std::vector<Migration>& persona_engine_migrations() {
    static const std::vector<Migration> migrations = {
        {"pe_0001_initial",
         {
             R"(CREATE TABLE IF NOT EXISTS personas (
        id TEXT PRIMARY KEY,
        name TEXT NOT NULL,
        display_name TEXT NOT NULL DEFAULT '',
        description TEXT NOT NULL,
        traits TEXT NOT NULL,
        speech_style TEXT NOT NULL,
        skill_template TEXT NOT NULL DEFAULT '',
        learned_notes TEXT NOT NULL DEFAULT '[]',
        created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL
      ))",
             R"(CREATE TABLE IF NOT EXISTS persona_assignments (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        persona_id TEXT NOT NULL,
        session_id TEXT NOT NULL,
        assigned_at INTEGER NOT NULL,
        released_at INTEGER
      ))",
             R"(CREATE TABLE IF NOT EXISTS persona_feedback (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        persona_id TEXT NOT NULL,
        session_id TEXT,
        ts INTEGER NOT NULL,
        kind TEXT NOT NULL,
        delta TEXT NOT NULL,
        detail TEXT
      ))",
             R"(CREATE TABLE IF NOT EXISTS rules (
        id TEXT PRIMARY KEY,
        description TEXT,
        trigger_type TEXT NOT NULL,
        tick_sec INTEGER,
        event_kind TEXT,
        conditions TEXT NOT NULL DEFAULT '[]',
        instructions TEXT NOT NULL,
        target TEXT,
        cooldown_sec INTEGER NOT NULL DEFAULT 60,
        last_fired_at INTEGER,
        enabled INTEGER NOT NULL DEFAULT 1,
        added_at INTEGER NOT NULL,
        added_by TEXT NOT NULL,
        removed_at INTEGER,
        removed_by TEXT,
        removed_reason TEXT
      ))",
             R"(CREATE TABLE IF NOT EXISTS rule_log (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        ts INTEGER NOT NULL,
        rule_id TEXT,
        action TEXT NOT NULL,
        detail TEXT,
        actor TEXT NOT NULL
      ))",
             "CREATE INDEX IF NOT EXISTS idx_persona_assignment_session ON persona_assignments(session_id, released_at)",
             "CREATE INDEX IF NOT EXISTS idx_rule_log_ts ON rule_log(ts)",
         }},
        // restart-recovery: per-session fire counter を永続化し、再起動越しに safety cap を効かせる。
        {"pe_0002_session_fires",
         {
             R"(CREATE TABLE IF NOT EXISTS session_rule_fires (
        session_id TEXT NOT NULL,
        rule_id TEXT NOT NULL,
        count INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY (session_id, rule_id)
      ))",
             "CREATE INDEX IF NOT EXISTS idx_session_rule_fires_session ON session_rule_fires(session_id)",
         }},
    };
    return migrations;
}

void apply_persona_engine_migrations(sql::Database& db) {
    db.exec("CREATE TABLE IF NOT EXISTS _pe_migrations (id TEXT PRIMARY KEY, applied_at INTEGER NOT NULL)");
    // TS は BEGIN … COMMIT / 失敗で ROLLBACK して投げ直す。transaction() も同じ。
    db.transaction([&] {
        for (const auto& m : persona_engine_migrations()) {
            if (db.get("SELECT 1 FROM _pe_migrations WHERE id = ?", {m.id})) continue;
            for (const auto& stmt : m.sql) {
                try {
                    db.exec(stmt);
                } catch (const sql::Error&) {
                    /* idempotent: already applied */
                }
            }
            db.run("INSERT INTO _pe_migrations (id, applied_at) VALUES (?, ?)", {m.id, time::now_ms()});
        }
    });
}

} // namespace di::persona_engine::db
