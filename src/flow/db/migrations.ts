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
  {
    // ペルソナプール (永続) + ユーザ嗜好ベクトル。
    //  - flow_persona: 学習データ別に用意/合成した永続ペルソナ。affect_vector で嗜好近傍検索 (憑依/壁打ち相手)。
    //  - flow_user_affect: ユーザ (Discord id 等) の「ゲームに望む感情/体験」ベクトル。憑依の検索キー。
    // ベクトルは sentiment-vector.ts の 20 次元 (JSON 配列) を格納する。
    id: "flow_0005_persona_pool",
    sql: [
      `CREATE TABLE IF NOT EXISTS flow_persona (
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
      )`,
      `CREATE TABLE IF NOT EXISTS flow_user_affect (
        user_key TEXT PRIMARY KEY,
        label TEXT,
        desired_text TEXT NOT NULL DEFAULT '',
        vector_json TEXT NOT NULL DEFAULT '[]',
        updated_at INTEGER NOT NULL
      )`,
      `CREATE INDEX IF NOT EXISTS idx_flow_persona_origin ON flow_persona(origin, archived)`,
      `CREATE INDEX IF NOT EXISTS idx_flow_persona_source ON flow_persona(learning_source)`,
    ],
  },
  {
    // C1 実在ユーザ採用: 話者アンカー (ext:source:authorId) で upsert + 典型度。
    // ALTER ADD COLUMN の後に INDEX (既存 DB で no such column を避ける共通ルール)。
    id: "flow_0006_persona_adopt",
    sql: [
      `ALTER TABLE flow_persona ADD COLUMN source_speaker_id TEXT`,
      // 母集団平均からの近さ (cosine, 高い=典型 / 低い=外れ)。「平均値グループにいるか」の判断材料。
      `ALTER TABLE flow_persona ADD COLUMN typicality REAL`,
      `CREATE INDEX IF NOT EXISTS idx_flow_persona_speaker ON flow_persona(source_speaker_id)`,
    ],
  },
  {
    // C2-b 母数推定値の永続化: 合成ペルソナ (origin=generated) の所属クラスタが実分布で持つ
    // 母数判定 (大/小) と実近傍比率を flow_persona に書き戻す (estimatePopulations の結果)。
    // ALTER ADD COLUMN の後に INDEX (既存 DB で no such column を避ける共通ルール)。
    id: "flow_0007_persona_population",
    sql: [
      // 母数判定 ("大" | "小")。NULL = 未推定。
      `ALTER TABLE flow_persona ADD COLUMN population_verdict TEXT`,
      // 所属クラスタ centroid の実分布近傍比率 (realNeighbors / 実分布総数)。
      `ALTER TABLE flow_persona ADD COLUMN population_ratio REAL`,
      // 推定実行時刻 (epoch ms)。再推定で上書き。
      `ALTER TABLE flow_persona ADD COLUMN population_estimated_at INTEGER`,
    ],
  },
  {
    // affect 解像度向上 (Issue #125): C1 採用の特徴量を拡充。
    //  - polarity_bias: 極性の片寄り |pos-neg|/total (0=均衡 / 1=一方向)。
    //  - affect_dispersion: ゲーム間 affect のばらつき (per-game ベクトルの平均対距離)。
    // 中立寄りに潰れたベクトル同士を分離する追加特徴量 (typicality cosine 単独の補完)。
    // ALTER ADD COLUMN の後に INDEX (既存 DB で no such column を避ける共通ルール)。
    id: "flow_0008_persona_affect_features",
    sql: [
      `ALTER TABLE flow_persona ADD COLUMN polarity_bias REAL`,
      `ALTER TABLE flow_persona ADD COLUMN affect_dispersion REAL`,
    ],
  },
  {
    // 憑依 (B, item4): その発話で演じていたデータ由来ペルソナの露出名を残す。
    // 露出面の表示名「名前 (ロール/憑依)」と WebUI 復元に使う。NULL = 憑依なし。
    id: "flow_0009_utterance_possession",
    sql: [`ALTER TABLE flow_utterance ADD COLUMN possession_name TEXT`],
  },
  {
    // LLM コスト計装: プロンプトキャッシュ usage + コスト推定を llm_call_log に追加。
    //  - cache_read/creation_input_tokens: backend が返す時のみ (claude-cli json / anthropic)。
    //  - cost_usd: claude-cli(サブスク) は等価 API 換算推定、他 backend は通常 NULL。
    // ALTER ADD COLUMN のみ (新規 INDEX 不要)。既存 DB で no such column を避ける共通ルール。
    id: "flow_0010_llm_cost",
    sql: [
      `ALTER TABLE llm_call_log ADD COLUMN cache_read_input_tokens INTEGER`,
      `ALTER TABLE llm_call_log ADD COLUMN cache_creation_input_tokens INTEGER`,
      `ALTER TABLE llm_call_log ADD COLUMN cost_usd REAL`,
    ],
  },
  {
    // 結論一覧 (まとめ) の読み取りキャッシュ。学習ビューの「結論」タブは従来 KG (481MB) を
    // 開いて行ごとにサブクエリを撃っていて重かった。議論が収束するたびにこの行を
    // write-through 更新し、一覧閲覧はこのキャッシュだけ読む (KG 非タッチ)。論述データ
    // 詳細 / md エクスポートは従来通りグラフ (KG) を引く。
    //  - discussion_volume: 議論ボリューム (発話数)。収束時に write-through 更新。
    //  - material_count: 話題に紐づく外部クロール材料の件数。KG を引く重い計算なので
    //    別途 (build:conclusion-cache) でまとめて算出する。未算出は -1。
    id: "flow_0011_conclusion_cache",
    sql: [
      `CREATE TABLE IF NOT EXISTS conclusion_cache (
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
      )`,
      `CREATE INDEX IF NOT EXISTS idx_conclusion_cache_updated ON conclusion_cache(updated_at DESC)`,
    ],
  },
  {
    // ディスカッションペーパー本文の markdown 正本化 (ハイブリッド源泉モデル)。
    //  - discussion_paper.body_md: 議論ブリーフ本文の正本 markdown (各 LLM が直接参照)。
    //    NULL の旧行は構造化フィールドから派生 (buildPaperSystem の従来組み立て) で後方互換。
    //  - discussion_paper_revision: Web の Notion 風編集の版履歴 (session 単位・追記専用)。
    //    「戻す」は 1 手前の body_md を新リビジョンとして積み直す (前進のみ・履歴は失わない)。
    // ALTER ADD COLUMN の後に CREATE TABLE / INDEX (既存 DB で no such column を避ける共通ルール)。
    id: "flow_0012_paper_body_md",
    sql: [
      `ALTER TABLE discussion_paper ADD COLUMN body_md TEXT`,
      `CREATE TABLE IF NOT EXISTS discussion_paper_revision (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        session_id TEXT NOT NULL,
        rev INTEGER NOT NULL,
        body_md TEXT NOT NULL,
        change_summary TEXT NOT NULL DEFAULT '',
        origin TEXT NOT NULL DEFAULT 'manual',
        created_at INTEGER NOT NULL
      )`,
      `CREATE INDEX IF NOT EXISTS idx_paper_revision_session ON discussion_paper_revision(session_id, rev)`,
    ],
  },
  {
    // ペーパーのライフサイクル状態。'draft'=編集中(未確定) / 'started'=議論開始済み。
    //  - 編集ゲートで草案が ready になった時点で 'draft' 行を作り、議論一覧に「下書き」として出す。
    //  - 確定 (approve→runFlow の persistPaper) で同 session 行を 'started' に upsert する。
    // 既存行は 'started' 既定 (= 旧来は開始時のみ永続だったため)。
    id: "flow_0013_paper_status",
    sql: [`ALTER TABLE discussion_paper ADD COLUMN status TEXT NOT NULL DEFAULT 'started'`],
  },
  {
    // 議論一覧の表示タイトル用キャッシュ。重い結論本文参照を一覧描画から外す。
    id: "flow_0014_discussion_title_cache",
    sql: [
      `CREATE TABLE IF NOT EXISTS discussion_title_cache (
        session_id TEXT PRIMARY KEY,
        title TEXT NOT NULL,
        source TEXT NOT NULL DEFAULT 'paper',
        updated_at INTEGER NOT NULL
      )`,
      `CREATE INDEX IF NOT EXISTS idx_discussion_title_cache_updated ON discussion_title_cache(updated_at DESC)`,
      `INSERT OR IGNORE INTO discussion_title_cache (session_id, title, source, updated_at)
       SELECT session_id, theme, 'paper', updated_at FROM discussion_paper`,
      `INSERT OR REPLACE INTO discussion_title_cache (session_id, title, source, updated_at)
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
          AND COALESCE(TRIM(fc.summary), '') != ''`,
    ],
  },
  {
    // 共通議論一覧用メタデータ。AI議論/チャット議論の一覧出し分けに使う。
    id: "flow_0015_discussion_title_cache_meta",
    sql: [
      `ALTER TABLE discussion_title_cache ADD COLUMN discussion_type TEXT NOT NULL DEFAULT 'discussion'`,
      `ALTER TABLE discussion_title_cache ADD COLUMN origin_ui TEXT NOT NULL DEFAULT 'ai'`,
      `UPDATE discussion_title_cache
          SET discussion_type = COALESCE((SELECT dp.flow FROM discussion_paper dp WHERE dp.session_id = discussion_title_cache.session_id), discussion_type)`,
      `UPDATE discussion_title_cache
          SET origin_ui = CASE
            WHEN discussion_type = 'sparring' THEN 'chat'
            ELSE 'ai'
          END`,
      `CREATE INDEX IF NOT EXISTS idx_discussion_title_cache_origin ON discussion_title_cache(origin_ui, discussion_type, updated_at DESC)`,
    ],
  },
  {
    // 改善フロー スコアリング再設計 (respec PR-C / improvement.md (2) 2026-07-02 改訂):
    //  - method: スコアの算出方式。'effect-predict' (LLM 効果予測) / 'lexicon-fallback'
    //    (LLM 失敗時の意見単位 degrade) / 'lexicon' (旧データ既定)。degrade の可視化用。
    //  - detail_json: 効果予測の changes[] (次元別 delta + 理由)。監査・可視化用。
    // ALTER ADD COLUMN のみ (新規 INDEX 不要)。既存 DB で no such column を避ける共通ルール。
    id: "flow_0016_improvement_score_method",
    sql: [
      `ALTER TABLE improvement_score ADD COLUMN method TEXT NOT NULL DEFAULT 'lexicon'`,
      `ALTER TABLE improvement_score ADD COLUMN detail_json TEXT`,
    ],
  },
  {
    // 議論適性ゲート (09-paper-gate-debatability) の評価結果。
    // 議論不適のまま強行したときに JSON で記録する (null 可 = ゲート未実施/適性あり)。
    // ALTER ADD COLUMN のみ (新規 INDEX 不要)。
    // (旧 id flow_0016_paper_debatability — PR-C の flow_0016 と並んだため 0017 へ採番し直し。
    //  merge 前のブランチ内のみで使われた id なので rename 安全)
    id: "flow_0017_paper_debatability",
    sql: [`ALTER TABLE discussion_paper ADD COLUMN debatability_json TEXT`],
  },
];

function isIgnorableMigrationError(stmt: string, error: unknown): boolean {
  if (!/^\s*ALTER\s+TABLE\s+/i.test(stmt)) return false;
  const message = error instanceof Error ? error.message : String(error);
  return /duplicate column name/i.test(message);
}

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
        } catch (e) {
          if (!isIgnorableMigrationError(stmt, e)) {
            throw e;
          }
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
