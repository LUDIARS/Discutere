/**
 * Kuzu グラフ DB クライアント — spec/core/KUZU-MIGRATION.md §3/§7。
 *
 * `KuzuClient`（名前だけ Kuzu で実体は better-sqlite3）の後継。
 * KG 本体（ノード + 関係）を本物の Kuzu embedded DB で管理する。
 * index/events/embeddings は引き続き SQLite 側（KuzuClient）。
 *
 * リソース寿命: `close()` を全経路で確実に呼ぶこと（RULE_CODE §4）。
 */

import * as kuzu from "kuzu";
import path from "node:path";
import fs from "node:fs";

// §3 ノードテーブル DDL（1 文ずつ実行するため ; で区切る）
const NODE_DDL_STMTS = [
  `CREATE NODE TABLE IF NOT EXISTS Person(
     id STRING, workspace_id STRING, name STRING, role STRING,
     created_at INT64, updated_at INT64, PRIMARY KEY(id))`,
  `CREATE NODE TABLE IF NOT EXISTS Game(
     id STRING, workspace_id STRING, title STRING, genre STRING,
     created_at INT64, updated_at INT64, PRIMARY KEY(id))`,
  `CREATE NODE TABLE IF NOT EXISTS Mechanic(
     id STRING, workspace_id STRING, name STRING, description STRING,
     intends STRING, intended_affect STRING,
     created_at INT64, updated_at INT64, PRIMARY KEY(id))`,
  `CREATE NODE TABLE IF NOT EXISTS Aesthetic(
     id STRING, workspace_id STRING, name STRING, description STRING,
     created_at INT64, updated_at INT64, PRIMARY KEY(id))`,
  `CREATE NODE TABLE IF NOT EXISTS Affect(
     id STRING, workspace_id STRING, mood STRING, score DOUBLE,
     valence STRING, vocabulary_status STRING,
     created_at INT64, updated_at INT64, PRIMARY KEY(id))`,
  `CREATE NODE TABLE IF NOT EXISTS PlayContext(
     id STRING, workspace_id STRING, platform STRING, mode STRING,
     created_at INT64, updated_at INT64, PRIMARY KEY(id))`,
  `CREATE NODE TABLE IF NOT EXISTS Session(
     id STRING, workspace_id STRING, title STRING, started_at INT64,
     ended_at INT64, mode STRING, scene STRING,
     created_at INT64, updated_at INT64, PRIMARY KEY(id))`,
  `CREATE NODE TABLE IF NOT EXISTS Utterance(
     id STRING, workspace_id STRING, raw_content STRING,
     normalized_content STRING, posted_at INT64,
     created_at INT64, updated_at INT64, PRIMARY KEY(id))`,
  `CREATE NODE TABLE IF NOT EXISTS Reaction(
     id STRING, workspace_id STRING, reaction_type STRING,
     intensity DOUBLE, created_at INT64, updated_at INT64, PRIMARY KEY(id))`,
  `CREATE NODE TABLE IF NOT EXISTS DesignGap(
     id STRING, workspace_id STRING, title STRING, description STRING,
     status STRING, gap_in STRING, expected_affect STRING,
     observed_affect STRING, evidence_json STRING,
     created_at INT64, updated_at INT64, PRIMARY KEY(id))`,
  `CREATE NODE TABLE IF NOT EXISTS Hypothesis(
     id STRING, workspace_id STRING, statement STRING, status STRING,
     integrated BOOLEAN, validated_by_emotion BOOLEAN,
     stale_flagged BOOLEAN, refs_json STRING,
     created_at INT64, updated_at INT64, PRIMARY KEY(id))`,
  // 外部発話 (YouTube/Reddit 等) の感情ノード — ExternalSentimentTransform が書く。
  `CREATE NODE TABLE IF NOT EXISTS ExternalSentiment(
     id STRING, game_slug STRING, source STRING, native_id STRING,
     polarity STRING, score DOUBLE, confidence DOUBLE, tier INT64,
     fetched_at STRING, PRIMARY KEY(id))`,
];

// §3 関係テーブル DDL
const REL_DDL_STMTS = [
  `CREATE REL TABLE IF NOT EXISTS SPOKEN_BY(FROM Utterance TO Person)`,
  `CREATE REL TABLE IF NOT EXISTS IN_SESSION(FROM Utterance TO Session, FROM Affect TO Session)`,
  `CREATE REL TABLE IF NOT EXISTS RESPONDS_TO(FROM Utterance TO Utterance)`,
  `CREATE REL TABLE IF NOT EXISTS REACTS_ON(FROM Reaction TO Utterance)`,
  `CREATE REL TABLE IF NOT EXISTS REACTED_BY(FROM Reaction TO Person)`,
  `CREATE REL TABLE IF NOT EXISTS OF_GAME(FROM Mechanic TO Game, FROM Aesthetic TO Game, FROM PlayContext TO Game)`,
  `CREATE REL TABLE IF NOT EXISTS ABOUT(FROM Affect TO Mechanic, FROM Affect TO Aesthetic)`,
  `CREATE REL TABLE IF NOT EXISTS IN_GAME(FROM DesignGap TO Game)`,
  `CREATE REL TABLE IF NOT EXISTS ADDRESSES(FROM Hypothesis TO DesignGap)`,
  `CREATE REL TABLE IF NOT EXISTS REFINES(FROM Utterance TO Mechanic)`,
  `CREATE REL TABLE IF NOT EXISTS INTEGRATES_AS(FROM Hypothesis TO Mechanic)`,
  `CREATE REL TABLE IF NOT EXISTS SENTIMENT_FOR(FROM ExternalSentiment TO Game)`,
];

export class KuzuGraphClient {
  private readonly db: kuzu.Database;
  private readonly conn: kuzu.Connection;
  readonly dbDirectory: string;

  constructor(dbDir: string) {
    this.dbDirectory = path.resolve(dbDir);
    fs.mkdirSync(path.dirname(this.dbDirectory), { recursive: true });
    this.db = new kuzu.Database(this.dbDirectory);
    this.conn = new kuzu.Connection(this.db);
  }

  /**
   * DDL を冪等適用する。起動時に 1 回呼ぶ。
   * 既存テーブルは IF NOT EXISTS でスキップされる。
   */
  async applySchema(): Promise<void> {
    for (const stmt of [...NODE_DDL_STMTS, ...REL_DDL_STMTS]) {
      const result = await this.conn.query(stmt);
      // query は単一文なので QueryResult (配列でない)
      (Array.isArray(result) ? result : [result]).forEach((r) => r.close());
    }
  }

  /**
   * パラメタなし Cypher を実行し全行を返す。
   * DDL や単純な MATCH に使う。
   */
  async query(cypher: string): Promise<Record<string, kuzu.KuzuValue>[]> {
    const result = await this.conn.query(cypher);
    const single = Array.isArray(result) ? result[0] : result;
    const rows = await single.getAll();
    (Array.isArray(result) ? result : [result]).forEach((r) => r.close());
    return rows;
  }

  /**
   * パラメタ付き Cypher を実行する（MERGE/SET 等）。
   * `prepare` + `execute` 経由でパラメタをバインドする。
   */
  async run(cypher: string, params: Record<string, kuzu.KuzuValue>): Promise<void> {
    const stmt = await this.conn.prepare(cypher);
    const result = await this.conn.execute(stmt, params);
    (Array.isArray(result) ? result : [result]).forEach((r) => r.close());
  }

  /** ノード件数を type 別に返す（検証用）。 */
  async nodeCountByType(): Promise<Record<string, number>> {
    const tables = [
      "Person", "Game", "Mechanic", "Aesthetic", "Affect",
      "PlayContext", "Session", "Utterance", "Reaction", "DesignGap", "Hypothesis",
      "ExternalSentiment",
    ];
    const counts: Record<string, number> = {};
    for (const t of tables) {
      try {
        const rows = await this.query(`MATCH (n:${t}) RETURN count(n) AS cnt`);
        counts[t] = Number((rows[0] as { cnt: number })?.cnt ?? 0);
      } catch {
        counts[t] = -1;
      }
    }
    return counts;
  }

  close(): void {
    this.conn.closeSync();
    this.db.closeSync();
  }
}

/** KuzuGraphClient を生成し DDL を適用して返す。 */
export async function openKuzuGraph(dbDir: string): Promise<KuzuGraphClient> {
  const client = new KuzuGraphClient(dbDir);
  await client.applySchema();
  return client;
}
