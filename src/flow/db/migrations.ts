/**
 * フロー共通 DB マイグレーション (OVERVIEW §8, T1 spec §3)。
 *
 * persona-engine の applyPersonaEngineMigrations パターンに倣い、
 * `_flow_migrations` テーブルで管理する。discutere.db に追記する。
 * INDEX は ALTER ADD COLUMN の後に冪等発行する (共通ルール)。
 */

import type Database from "better-sqlite3";

const MIGRATIONS: Array<{ id: string; sql: string[] }> = [
  {
    id: "flow_0001_initial",
    sql: [
      // 1 議論 = 1 ディスカッションペーパー
      `CREATE TABLE IF NOT EXISTS discussion_paper (
        id TEXT PRIMARY KEY,
        flow TEXT NOT NULL,
        session_id TEXT NOT NULL,
        theme TEXT NOT NULL,
        tags_json TEXT NOT NULL DEFAULT '[]',
        mechanics_json TEXT NOT NULL DEFAULT '[]',
        supplement TEXT NOT NULL DEFAULT '',
        created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL
      )`,
      // ラウンド追記 (append-only)
      `CREATE TABLE IF NOT EXISTS discussion_paper_round (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        paper_id TEXT NOT NULL,
        round INTEGER NOT NULL,
        summary TEXT NOT NULL DEFAULT '',
        aufhebung_json TEXT NOT NULL DEFAULT '[]',
        created_at INTEGER NOT NULL
      )`,
      // 投票 (T3 で書込)
      `CREATE TABLE IF NOT EXISTS vote (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        session_id TEXT NOT NULL,
        round INTEGER NOT NULL,
        voter_index INTEGER NOT NULL,
        chosen_utterance_id TEXT,
        created_at INTEGER NOT NULL
      )`,
      // LLM 呼び出しコスト + transcript 統合ログ (内部検証用)
      `CREATE TABLE IF NOT EXISTS llm_call_log (
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
      )`,
      // INDEX は CREATE TABLE の後
      `CREATE INDEX IF NOT EXISTS idx_discussion_paper_session ON discussion_paper(session_id)`,
      `CREATE INDEX IF NOT EXISTS idx_discussion_paper_flow ON discussion_paper(flow)`,
      `CREATE INDEX IF NOT EXISTS idx_paper_round_paper ON discussion_paper_round(paper_id)`,
      `CREATE INDEX IF NOT EXISTS idx_vote_session ON vote(session_id, round)`,
      `CREATE INDEX IF NOT EXISTS idx_llm_call_log_session ON llm_call_log(session_id)`,
      `CREATE INDEX IF NOT EXISTS idx_llm_call_log_flow ON llm_call_log(flow, created_at)`,
    ],
  },
  {
    id: "flow_0002_utterance",
    sql: [
      // 議論フロー内の発話 (1 ターン = 1 行)
      `CREATE TABLE IF NOT EXISTS flow_utterance (
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
      )`,
      `CREATE INDEX IF NOT EXISTS idx_flow_utterance_session ON flow_utterance(session_id, round)`,
      `CREATE INDEX IF NOT EXISTS idx_flow_utterance_paper ON flow_utterance(paper_id)`,
    ],
  },
  {
    id: "flow_0003_conclusion",
    sql: [
      // 議論の結論 (1 セッション = 1 行)
      `CREATE TABLE IF NOT EXISTS flow_conclusion (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        session_id TEXT NOT NULL,
        paper_id TEXT NOT NULL,
        summary TEXT NOT NULL DEFAULT '',
        aufhebung_json TEXT NOT NULL DEFAULT '[]',
        top_utterance_ids_json TEXT NOT NULL DEFAULT '[]',
        concluded INTEGER NOT NULL DEFAULT 0,
        created_at INTEGER NOT NULL
      )`,
      `CREATE UNIQUE INDEX IF NOT EXISTS idx_flow_conclusion_session ON flow_conclusion(session_id)`,
    ],
  },
  {
    id: "flow_0004_improvement_score",
    sql: [
      // 改善フローの機械スコア (1 ラウンド × 意見 = 1 行)。design_gap 射影スコアを監査用に残す
      `CREATE TABLE IF NOT EXISTS improvement_score (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        session_id TEXT NOT NULL,
        round INTEGER NOT NULL,
        utterance_id TEXT NOT NULL,
        score REAL NOT NULL,
        is_winner INTEGER NOT NULL DEFAULT 0,
        created_at INTEGER NOT NULL
      )`,
      `CREATE INDEX IF NOT EXISTS idx_improvement_score_session ON improvement_score(session_id, round)`,
    ],
  },
];

export function applyFlowMigrations(db: Database.Database): void {
  db.exec(
    `CREATE TABLE IF NOT EXISTS _flow_migrations (id TEXT PRIMARY KEY, applied_at INTEGER NOT NULL)`
  );
  db.exec("BEGIN");
  try {
    for (const m of MIGRATIONS) {
      const exists = db
        .prepare("SELECT 1 FROM _flow_migrations WHERE id = ?")
        .get(m.id) as { 1: number } | undefined;
      if (exists) continue;
      for (const stmt of m.sql) {
        try {
          db.exec(stmt);
        } catch {
          /* idempotent: already applied */
        }
      }
      db.prepare("INSERT INTO _flow_migrations (id, applied_at) VALUES (?, ?)").run(
        m.id,
        Date.now()
      );
    }
    db.exec("COMMIT");
  } catch (e) {
    db.exec("ROLLBACK");
    throw e;
  }
}
