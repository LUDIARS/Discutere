#include "flow/db/migrations.hpp"

#include "base/strings.hpp"
#include "base/time.hpp"

namespace di::flow::db {

const std::vector<FlowMigration>& flow_migrations() {
    static const std::vector<FlowMigration> migrations = {
        {"flow_0001_initial",
         {
             R"SQL(CREATE TABLE IF NOT EXISTS discussion_paper (
        id TEXT PRIMARY KEY,
        flow TEXT NOT NULL,
        session_id TEXT NOT NULL,
        theme TEXT NOT NULL,
        tags_json TEXT NOT NULL DEFAULT '[]',
        mechanics_json TEXT NOT NULL DEFAULT '[]',
        supplement TEXT NOT NULL DEFAULT '',
        created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL
      ))SQL",
             R"SQL(CREATE TABLE IF NOT EXISTS discussion_paper_round (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        paper_id TEXT NOT NULL,
        round INTEGER NOT NULL,
        summary TEXT NOT NULL DEFAULT '',
        aufhebung_json TEXT NOT NULL DEFAULT '[]',
        created_at INTEGER NOT NULL
      ))SQL",
             R"SQL(CREATE TABLE IF NOT EXISTS vote (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        session_id TEXT NOT NULL,
        round INTEGER NOT NULL,
        voter_index INTEGER NOT NULL,
        chosen_utterance_id TEXT,
        created_at INTEGER NOT NULL
      ))SQL",
             R"SQL(CREATE TABLE IF NOT EXISTS llm_call_log (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        flow TEXT NOT NULL,
        session_id TEXT NOT NULL,
        round INTEGER,
        turn INTEGER,
        role TEXT,
        persona TEXT,
        location TEXT NOT NULL,
        model TEXT,
        backend TEXT,
        latency_ms INTEGER,
        input_tokens INTEGER,
        output_tokens INTEGER,
        prompt TEXT,
        response TEXT,
        created_at INTEGER NOT NULL
      ))SQL",
             R"SQL(CREATE INDEX IF NOT EXISTS idx_discussion_paper_session ON discussion_paper(session_id))SQL",
             R"SQL(CREATE INDEX IF NOT EXISTS idx_discussion_paper_flow ON discussion_paper(flow))SQL",
             R"SQL(CREATE INDEX IF NOT EXISTS idx_paper_round_paper ON discussion_paper_round(paper_id))SQL",
             R"SQL(CREATE INDEX IF NOT EXISTS idx_vote_session ON vote(session_id, round))SQL",
             R"SQL(CREATE INDEX IF NOT EXISTS idx_llm_call_log_session ON llm_call_log(session_id))SQL",
             R"SQL(CREATE INDEX IF NOT EXISTS idx_llm_call_log_flow ON llm_call_log(flow, created_at))SQL"}},
        {"flow_0002_utterance",
         {
             R"SQL(CREATE TABLE IF NOT EXISTS flow_utterance (
        id TEXT PRIMARY KEY,
        session_id TEXT NOT NULL,
        paper_id TEXT NOT NULL,
        round INTEGER NOT NULL,
        turn INTEGER NOT NULL,
        persona_id TEXT NOT NULL,
        persona_name TEXT NOT NULL,
        role TEXT NOT NULL,
        stance TEXT NOT NULL DEFAULT 'neutral',
        text TEXT NOT NULL,
        is_error INTEGER NOT NULL DEFAULT 0,
        created_at INTEGER NOT NULL
      ))SQL",
             R"SQL(CREATE INDEX IF NOT EXISTS idx_flow_utterance_session ON flow_utterance(session_id, round))SQL",
             R"SQL(CREATE INDEX IF NOT EXISTS idx_flow_utterance_paper ON flow_utterance(paper_id))SQL"}},
        {"flow_0003_conclusion",
         {
             R"SQL(CREATE TABLE IF NOT EXISTS flow_conclusion (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        session_id TEXT NOT NULL,
        paper_id TEXT NOT NULL,
        summary TEXT NOT NULL DEFAULT '',
        aufhebung_json TEXT NOT NULL DEFAULT '[]',
        top_utterance_ids_json TEXT NOT NULL DEFAULT '[]',
        concluded INTEGER NOT NULL DEFAULT 0,
        created_at INTEGER NOT NULL
      ))SQL",
             R"SQL(CREATE UNIQUE INDEX IF NOT EXISTS idx_flow_conclusion_session ON flow_conclusion(session_id))SQL"}},
        {"flow_0004_improvement_score",
         {
             R"SQL(CREATE TABLE IF NOT EXISTS improvement_score (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        session_id TEXT NOT NULL,
        round INTEGER NOT NULL,
        utterance_id TEXT NOT NULL,
        score REAL NOT NULL,
        is_winner INTEGER NOT NULL DEFAULT 0,
        created_at INTEGER NOT NULL
      ))SQL",
             R"SQL(CREATE INDEX IF NOT EXISTS idx_improvement_score_session ON improvement_score(session_id, round))SQL"}},
        {"flow_0005_persona_pool",
         {
             R"SQL(CREATE TABLE IF NOT EXISTS flow_persona (
        id TEXT PRIMARY KEY,
        name TEXT NOT NULL,
        role TEXT NOT NULL DEFAULT 'opinion',
        speech_style TEXT NOT NULL DEFAULT '',
        traits_json TEXT NOT NULL DEFAULT '[]',
        affect_vector_json TEXT NOT NULL DEFAULT '[]',
        origin TEXT NOT NULL DEFAULT 'seed',
        parent_ids_json TEXT NOT NULL DEFAULT '[]',
        learning_source TEXT,
        label TEXT,
        model TEXT,
        archived INTEGER NOT NULL DEFAULT 0,
        created_at INTEGER NOT NULL
      ))SQL",
             R"SQL(CREATE INDEX IF NOT EXISTS idx_flow_persona_origin ON flow_persona(origin, archived))SQL",
             R"SQL(CREATE INDEX IF NOT EXISTS idx_flow_persona_source ON flow_persona(learning_source))SQL"}},
        {"flow_0006_persona_adopt",
         {
             R"SQL(ALTER TABLE flow_persona ADD COLUMN source_speaker_id TEXT)SQL",
             R"SQL(ALTER TABLE flow_persona ADD COLUMN typicality REAL)SQL",
             R"SQL(CREATE INDEX IF NOT EXISTS idx_flow_persona_speaker ON flow_persona(source_speaker_id))SQL"}},
        {"flow_0007_persona_population",
         {
             R"SQL(ALTER TABLE flow_persona ADD COLUMN population_verdict TEXT)SQL",
             R"SQL(ALTER TABLE flow_persona ADD COLUMN population_ratio REAL)SQL",
             R"SQL(ALTER TABLE flow_persona ADD COLUMN population_estimated_at INTEGER)SQL"}},
        {"flow_0008_persona_affect_features",
         {
             R"SQL(ALTER TABLE flow_persona ADD COLUMN polarity_bias REAL)SQL",
             R"SQL(ALTER TABLE flow_persona ADD COLUMN affect_dispersion REAL)SQL"}},
        {"flow_0009_utterance_possession",
         {
             R"SQL(ALTER TABLE flow_utterance ADD COLUMN possession_name TEXT)SQL"}},
        {"flow_0010_llm_cost",
         {
             R"SQL(ALTER TABLE llm_call_log ADD COLUMN cache_read_input_tokens INTEGER)SQL",
             R"SQL(ALTER TABLE llm_call_log ADD COLUMN cache_creation_input_tokens INTEGER)SQL",
             R"SQL(ALTER TABLE llm_call_log ADD COLUMN cost_usd REAL)SQL"}},
        {"flow_0011_conclusion_cache",
         {
             R"SQL(CREATE TABLE IF NOT EXISTS conclusion_cache (
        gap_id TEXT PRIMARY KEY,
        kind TEXT NOT NULL,
        session_id TEXT NOT NULL,
        title TEXT NOT NULL DEFAULT '',
        conclusion TEXT,
        utterance_count INTEGER NOT NULL DEFAULT 0,
        aufhebung_count INTEGER NOT NULL DEFAULT 0,
        discussion_volume INTEGER NOT NULL DEFAULT 0,
        material_count INTEGER NOT NULL DEFAULT -1,
        updated_at INTEGER NOT NULL,
        cached_at INTEGER NOT NULL
      ))SQL",
             R"SQL(CREATE INDEX IF NOT EXISTS idx_conclusion_cache_updated ON conclusion_cache(updated_at DESC))SQL"}},
        {"flow_0012_paper_body_md",
         {
             R"SQL(ALTER TABLE discussion_paper ADD COLUMN body_md TEXT)SQL",
             R"SQL(CREATE TABLE IF NOT EXISTS discussion_paper_revision (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        session_id TEXT NOT NULL,
        rev INTEGER NOT NULL,
        body_md TEXT NOT NULL,
        change_summary TEXT NOT NULL DEFAULT '',
        origin TEXT NOT NULL DEFAULT 'manual',
        created_at INTEGER NOT NULL
      ))SQL",
             R"SQL(CREATE INDEX IF NOT EXISTS idx_paper_revision_session ON discussion_paper_revision(session_id, rev))SQL"}},
        {"flow_0013_paper_status",
         {
             R"SQL(ALTER TABLE discussion_paper ADD COLUMN status TEXT NOT NULL DEFAULT 'started')SQL"}},
        {"flow_0014_discussion_title_cache",
         {
             R"SQL(CREATE TABLE IF NOT EXISTS discussion_title_cache (
        session_id TEXT PRIMARY KEY,
        title TEXT NOT NULL,
        source TEXT NOT NULL DEFAULT 'paper',
        updated_at INTEGER NOT NULL
      ))SQL",
             R"SQL(CREATE INDEX IF NOT EXISTS idx_discussion_title_cache_updated ON discussion_title_cache(updated_at DESC))SQL",
             R"SQL(INSERT OR IGNORE INTO discussion_title_cache (session_id, title, source, updated_at)
       SELECT session_id, theme, 'paper', updated_at FROM discussion_paper)SQL",
             R"SQL(INSERT OR REPLACE INTO discussion_title_cache (session_id, title, source, updated_at)
       SELECT dp.session_id,
              COALESCE(
                NULLIF(
                  TRIM(REPLACE(
                    CASE
                      WHEN INSTR(fc.summary, char(10)) > 0 THEN SUBSTR(fc.summary, 1, INSTR(fc.summary, char(10)) - 1)
                      ELSE fc.summary
                    END,
                    '【収束】',
                    ''
                  )),
                  ''
                ),
                dp.theme
              ),
              'conclusion',
              COALESCE(fc.created_at, dp.updated_at)
         FROM discussion_paper dp
         JOIN flow_conclusion fc ON fc.session_id = dp.session_id
        WHERE COALESCE(fc.concluded, 0) = 1
          AND COALESCE(TRIM(fc.summary), '') != '')SQL"}},
        {"flow_0015_discussion_title_cache_meta",
         {
             R"SQL(ALTER TABLE discussion_title_cache ADD COLUMN discussion_type TEXT NOT NULL DEFAULT 'discussion')SQL",
             R"SQL(ALTER TABLE discussion_title_cache ADD COLUMN origin_ui TEXT NOT NULL DEFAULT 'ai')SQL",
             R"SQL(UPDATE discussion_title_cache
          SET discussion_type = COALESCE((SELECT dp.flow FROM discussion_paper dp WHERE dp.session_id = discussion_title_cache.session_id), discussion_type))SQL",
             R"SQL(UPDATE discussion_title_cache
          SET origin_ui = CASE
            WHEN discussion_type = 'sparring' THEN 'chat'
            ELSE 'ai'
          END)SQL",
             R"SQL(CREATE INDEX IF NOT EXISTS idx_discussion_title_cache_origin ON discussion_title_cache(origin_ui, discussion_type, updated_at DESC))SQL"}},
        {"flow_0016_improvement_score_method",
         {
             R"SQL(ALTER TABLE improvement_score ADD COLUMN method TEXT NOT NULL DEFAULT 'lexicon')SQL",
             R"SQL(ALTER TABLE improvement_score ADD COLUMN detail_json TEXT)SQL"}},
        {"flow_0017_paper_debatability",
         {
             R"SQL(ALTER TABLE discussion_paper ADD COLUMN debatability_json TEXT)SQL"}},
        {"flow_0018_session_persona",
         {
             R"SQL(CREATE TABLE IF NOT EXISTS flow_session_persona (
        id TEXT PRIMARY KEY,
        session_id TEXT NOT NULL,
        name TEXT NOT NULL,
        role TEXT NOT NULL,
        stance TEXT NOT NULL,
        value_axis TEXT,
        core_claims_json TEXT,
        possession_name TEXT,
        created_at INTEGER NOT NULL
      ))SQL",
             R"SQL(CREATE INDEX IF NOT EXISTS idx_fsp_session ON flow_session_persona(session_id))SQL"}},
        {"flow_0019_utterance_act",
         {
             R"SQL(ALTER TABLE flow_utterance ADD COLUMN act TEXT)SQL",
             R"SQL(ALTER TABLE flow_utterance ADD COLUMN target_id TEXT)SQL"}},
        {"flow_0020_dialectic_core",
         {
             R"SQL(CREATE TABLE IF NOT EXISTS flow_issue (
        id TEXT PRIMARY KEY,
        session_id TEXT NOT NULL,
        title TEXT NOT NULL,
        ordinal INTEGER NOT NULL,
        source TEXT NOT NULL DEFAULT 'llm',
        status TEXT NOT NULL DEFAULT 'open',
        created_at INTEGER NOT NULL
      ))SQL",
             R"SQL(CREATE TABLE IF NOT EXISTS flow_position (
        id TEXT PRIMARY KEY,
        issue_id TEXT NOT NULL,
        persona_id TEXT NOT NULL,
        stance TEXT NOT NULL,
        claim TEXT NOT NULL,
        grounds_json TEXT NOT NULL,
        values_json TEXT NOT NULL,
        created_at INTEGER NOT NULL
      ))SQL",
             R"SQL(CREATE TABLE IF NOT EXISTS flow_tension (
        id TEXT PRIMARY KEY,
        issue_id TEXT NOT NULL,
        position_a TEXT NOT NULL,
        position_b TEXT NOT NULL,
        type TEXT NOT NULL,
        status TEXT NOT NULL DEFAULT 'open',
        resolution_note TEXT,
        created_at INTEGER NOT NULL
      ))SQL",
             R"SQL(CREATE TABLE IF NOT EXISTS flow_synthesis (
        id TEXT PRIMARY KEY,
        tension_id TEXT NOT NULL,
        preserves_json TEXT NOT NULL,
        negates_json TEXT NOT NULL,
        elevates TEXT,
        kind TEXT NOT NULL,
        text TEXT NOT NULL,
        ratification_json TEXT NOT NULL,
        status TEXT NOT NULL,
        revision INTEGER NOT NULL DEFAULT 0,
        created_at INTEGER NOT NULL
      ))SQL",
             R"SQL(CREATE INDEX IF NOT EXISTS idx_flow_issue_session ON flow_issue(session_id, ordinal))SQL",
             R"SQL(CREATE INDEX IF NOT EXISTS idx_flow_position_issue ON flow_position(issue_id))SQL",
             R"SQL(CREATE INDEX IF NOT EXISTS idx_flow_tension_issue ON flow_tension(issue_id))SQL",
             R"SQL(CREATE INDEX IF NOT EXISTS idx_flow_synthesis_tension ON flow_synthesis(tension_id))SQL"}},
        {"flow_0021_paper_review_info",
         {
             R"SQL(ALTER TABLE discussion_paper ADD COLUMN review_info_json TEXT)SQL"}},
        {"flow_0022_discussion_no",
         {
             R"SQL(ALTER TABLE discussion_paper ADD COLUMN discussion_no INTEGER)SQL",
             R"SQL(UPDATE discussion_paper
          SET discussion_no = (
            SELECT COUNT(*)
              FROM discussion_paper older
             WHERE older.created_at < discussion_paper.created_at
                OR (older.created_at = discussion_paper.created_at AND older.id <= discussion_paper.id)
          )
        WHERE discussion_no IS NULL)SQL",
             R"SQL(CREATE UNIQUE INDEX IF NOT EXISTS idx_discussion_paper_no ON discussion_paper(discussion_no) WHERE discussion_no IS NOT NULL)SQL"}},
        {"flow_0023_external_persona_identity",
         {
             R"SQL(ALTER TABLE flow_persona ADD COLUMN user_id TEXT)SQL",
             R"SQL(CREATE UNIQUE INDEX IF NOT EXISTS idx_flow_persona_user_id
         ON flow_persona(user_id) WHERE user_id IS NOT NULL)SQL",
             R"SQL(UPDATE flow_persona SET archived = 1 WHERE origin IN ('generated', 'synthesized'))SQL",
             R"SQL(DROP TABLE IF EXISTS flow_user_affect)SQL"}},
        {"flow_0024_persona_import_v2",
         {
             R"SQL(ALTER TABLE flow_persona ADD COLUMN preference_axes_json TEXT NOT NULL DEFAULT '{}')SQL",
             R"SQL(ALTER TABLE flow_persona ADD COLUMN attributes_json TEXT NOT NULL DEFAULT '{}')SQL",
             R"SQL(ALTER TABLE flow_persona ADD COLUMN aversions_json TEXT NOT NULL DEFAULT '[]')SQL",
             R"SQL(ALTER TABLE flow_persona ADD COLUMN mechanic_reactions_json TEXT NOT NULL DEFAULT '[]')SQL",
             R"SQL(ALTER TABLE flow_persona ADD COLUMN export_spec_version INTEGER)SQL"}},
        {"flow_0025_persona_bridge_assertion_nonce",
         {
             R"SQL(CREATE TABLE IF NOT EXISTS persona_bridge_assertion_nonce (
        jti TEXT PRIMARY KEY,
        expires_at INTEGER NOT NULL
      ))SQL",
             R"SQL(CREATE INDEX IF NOT EXISTS idx_persona_bridge_assertion_expiry
         ON persona_bridge_assertion_nonce(expires_at))SQL"}},
        {"flow_0026_persona_history_evidence",
         {
             R"SQL(CREATE TABLE IF NOT EXISTS flow_persona_history
        (user_id TEXT PRIMARY KEY, expires_at INTEGER NOT NULL, payload TEXT NOT NULL))SQL",
             R"SQL(CREATE INDEX IF NOT EXISTS idx_flow_persona_history_expiry ON flow_persona_history(expires_at))SQL"}},
        {"flow_0027_position_qualifier",
         {
             R"SQL(ALTER TABLE flow_position ADD COLUMN qualifier TEXT)SQL"}},
        {"flow_0028_alignment",
         {
             R"SQL(CREATE TABLE IF NOT EXISTS flow_alignment (
        id TEXT PRIMARY KEY,
        tension_id TEXT NOT NULL,
        experience TEXT NOT NULL,
        measure TEXT NOT NULL,
        reference_cases TEXT NOT NULL,
        agreed_json TEXT NOT NULL,
        open_json TEXT NOT NULL,
        text TEXT NOT NULL,
        created_at INTEGER NOT NULL
      ))SQL",
             R"SQL(CREATE INDEX IF NOT EXISTS idx_flow_alignment_tension ON flow_alignment(tension_id))SQL"}},
        {"flow_0029_optional_voices_rediscussion",
         {
             R"SQL(CREATE TABLE IF NOT EXISTS discord_flow_run (thread_id TEXT PRIMARY KEY, session_id TEXT NOT NULL, previous_session_id TEXT NOT NULL))SQL",
             R"SQL(CREATE TABLE IF NOT EXISTS flow_voice_preparation (session_id TEXT PRIMARY KEY, payload TEXT NOT NULL, updated_at INTEGER NOT NULL))SQL"}},
        {"flow_0030_paper_voices",
         {
             R"SQL(CREATE TABLE IF NOT EXISTS discussion_paper_voice (
      session_id TEXT NOT NULL,
      ordinal INTEGER NOT NULL,
      content TEXT NOT NULL,
      source TEXT NOT NULL,
      source_url TEXT,
      created_at INTEGER NOT NULL,
      PRIMARY KEY (session_id, ordinal)
    ))SQL"}},
        {"2026-10-18-conclusion-publication",
         {
             R"SQL(CREATE TABLE IF NOT EXISTS conclusion_publication (
    conclusion_id TEXT PRIMARY KEY,
    publishable INTEGER NOT NULL DEFAULT 0 CHECK(publishable IN (0,1)),
    scope TEXT NOT NULL DEFAULT 'private' CHECK(scope IN ('private','public','glab')),
    content_hash TEXT NOT NULL,
    updated_at INTEGER NOT NULL
  ))SQL"}},
        {"2026-10-23-proxy-profile",
         {
             R"SQL(CREATE TABLE IF NOT EXISTS flow_proxy_profile (
      speaker_key TEXT NOT NULL,
      genre TEXT NOT NULL,
      label TEXT NOT NULL,
      profile_version TEXT NOT NULL,
      content_hash TEXT NOT NULL,
      pack_json TEXT NOT NULL,
      confidence TEXT NOT NULL,
      expires_at INTEGER NOT NULL,
      revoked INTEGER NOT NULL DEFAULT 0 CHECK(revoked IN (0,1)),
      eval_status TEXT NOT NULL DEFAULT 'unevaluated' CHECK(eval_status IN ('unevaluated','passed','failed')),
      eval_json TEXT,
      imported_at INTEGER NOT NULL,
      updated_at INTEGER NOT NULL,
      PRIMARY KEY (speaker_key, genre)
    ))SQL",
             R"SQL(CREATE INDEX IF NOT EXISTS idx_flow_proxy_profile_label ON flow_proxy_profile(label, genre))SQL",
             R"SQL(ALTER TABLE flow_utterance ADD COLUMN claim_kind TEXT)SQL",
             R"SQL(ALTER TABLE flow_utterance ADD COLUMN evidence_ids_json TEXT)SQL"}},
    };
    return migrations;
}

bool is_ignorable_migration_error(std::string_view stmt, std::string_view message) {
    // /^\s*ALTER\s+TABLE\s+/i
    std::size_t i = 0;
    auto skip_space = [&] {
        const std::size_t start = i;
        while (i < stmt.size()) {
            std::size_t next = i;
            if (!str::is_js_space(str::next_codepoint(stmt, next))) break;
            i = next;
        }
        return i > start;
    };
    auto word = [&](std::string_view w) {
        if (stmt.size() - i < w.size() || !str::iequals_ascii(stmt.substr(i, w.size()), w)) return false;
        i += w.size();
        return true;
    };
    skip_space();
    if (!word("ALTER") || !skip_space() || !word("TABLE") || !skip_space()) return false;
    // /duplicate column name/i
    return str::contains(str::to_lower_ascii(message), "duplicate column name");
}

void apply_flow_migrations(sql::Database& db) {
    std::lock_guard lock(db.mutex());
    db.exec("CREATE TABLE IF NOT EXISTS _flow_migrations (id TEXT PRIMARY KEY, applied_at INTEGER NOT NULL)");
    db.exec("BEGIN");
    try {
        for (const auto& m : flow_migrations()) {
            if (db.get("SELECT 1 FROM _flow_migrations WHERE id = ?", {m.id})) continue;
            for (const auto& stmt : m.sql) {
                try {
                    db.exec(stmt);
                } catch (const sql::Error& e) {
                    if (!is_ignorable_migration_error(stmt, e.what())) throw;
                }
            }
            db.run("INSERT INTO _flow_migrations (id, applied_at) VALUES (?, ?)", {m.id, static_cast<double>(time::now_ms())});
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

} // namespace di::flow::db
