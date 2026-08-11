/**
 * クエリ埋め込みキャッシュ — 同期検索経路にベクトル検索を持ち込むための橋。
 *
 * `listRelevantExternalVoices` (context-provider interface) は同期だが、埋め込みの
 * 取得は HTTP (非同期)。そこで「クエリ (議題語) のベクトルだけ」を sidecar テーブル +
 * メモリにキャッシュし、
 *   - 同期読み (getCachedQueryVector): キャッシュ命中ならベクトルを返す
 *   - 非同期温め (warmQueryEmbedding): 未命中クエリを裏で埋め込んで保存
 * とする。議論は同一議題で何度も検索する (情報ゲート再評価・ペルソナ各ターン) ため、
 * 初回 miss → 2 回目以降 hit で実用上ほぼ常にハイブリッド検索になる。
 *
 * 声 (文書) 側のベクトルは scripts/build-voice-embeddings.ts が offline で
 * embeddings テーブルに構築する (このモジュールはクエリ側のみ)。
 */

import { createHash } from "node:crypto";

import BetterSqlite3 from "better-sqlite3";

import type { EmbeddingClient } from "./embedder.js";
import { isEmbeddingVector } from "./vector-math.js";

type RawDb = BetterSqlite3.Database;

/** キャッシュ行数の上限 (超過時は古い順に削除)。議題数はたかが知れているが暴走保険。 */
const MAX_ROWS = 1000;

/** プロセス内キャッシュ (core はコール毎に open/close されるため DB 跨ぎで共有する)。 */
const memory = new Map<string, number[]>();
/** 同一クエリの warm 多重発火を抑止する in-flight 管理。 */
const inflight = new Map<string, Promise<void>>();
/** 接続ごとの CREATE/旧 query_text 消去を一度だけにする。 */
const initializedDatabases = new WeakSet<RawDb>();
/** 同じ on-disk DB を開き直すたびに旧本文消去の UPDATE を繰り返さない。 */
const sanitizedDatabasePaths = new Set<string>();

function hashQuery(model: string, text: string): string {
  // モデルが変わればベクトル空間が変わるので、モデル名をキーに含める。
  return createHash("sha256").update(`${model}\n${text}`).digest("hex");
}

export function ensureQueryCacheTable(raw: RawDb): void {
  if (initializedDatabases.has(raw)) return;
  raw
    .prepare(
      `CREATE TABLE IF NOT EXISTS embedding_query_cache (
         query_hash TEXT PRIMARY KEY,
         query_text TEXT NOT NULL DEFAULT '',
         vector_json TEXT NOT NULL,
         created_at INTEGER NOT NULL
       )`
    )
    .run();
  if (raw.name === ":memory:" || !sanitizedDatabasePaths.has(raw.name)) {
    // 初期実装で保存した query_text も、キャッシュ利用時にデータ最小化する。
    raw.prepare("UPDATE embedding_query_cache SET query_text = '' WHERE query_text <> ''").run();
    if (raw.name !== ":memory:") sanitizedDatabasePaths.add(raw.name);
  }
  initializedDatabases.add(raw);
}

/** 正規化: 語順・重複・空白揺れでキャッシュキーが割れないようにする。 */
export function normalizeQueryText(terms: readonly string[]): string {
  return [...new Set(terms.map((t) => t.trim().toLowerCase()).filter(Boolean))]
    .sort()
    .join(" ");
}

/**
 * キャッシュ済みクエリベクトルを同期で引く。無ければ null (呼び出し側は
 * キーワード検索へ degrade しつつ warmQueryEmbedding を裏で発火する)。
 * @implements SPEC-VOICE-RAG-HYBRID-QUERY-CACHE
 */
export function getCachedQueryVector(raw: RawDb, model: string, queryText: string): number[] | null {
  const key = hashQuery(model, queryText);
  const mem = memory.get(key);
  if (mem) return mem;
  ensureQueryCacheTable(raw);
  const row = raw
    .prepare("SELECT vector_json FROM embedding_query_cache WHERE query_hash = ?")
    .get(key) as { vector_json: string } | undefined;
  if (!row) return null;
  try {
    const vec: unknown = JSON.parse(row.vector_json);
    if (!isEmbeddingVector(vec)) return null;
    memory.set(key, vec);
    return vec;
  } catch {
    return null;
  }
}

/**
 * 非同期 HTTP 中に呼び出し元が DB を閉じた場合は、同じ DB を短時間だけ開き直して保存する。
 * query_text はキャッシュキーに不要で、議題本文の永続化を避けるため空文字だけを書き込む。
 */
function persistQueryVector(raw: RawDb, databasePath: string, key: string, vec: number[]): void {
  const canReopen = databasePath !== ":memory:" && databasePath.length > 0;
  const db = raw.open ? raw : canReopen ? new BetterSqlite3(databasePath) : null;
  if (!db) return;
  const ownsDb = db !== raw;
  try {
    ensureQueryCacheTable(db);
    db
      .prepare(
        `INSERT INTO embedding_query_cache (query_hash, query_text, vector_json, created_at)
         VALUES (?, '', ?, ?)
         ON CONFLICT(query_hash) DO UPDATE SET
           query_text = '', vector_json = excluded.vector_json, created_at = excluded.created_at`
      )
      .run(key, JSON.stringify(vec), Date.now());
    const count = (db.prepare("SELECT COUNT(*) AS n FROM embedding_query_cache").get() as { n: number }).n;
    if (count > MAX_ROWS) {
      db
        .prepare(
          `DELETE FROM embedding_query_cache WHERE query_hash IN (
             SELECT query_hash FROM embedding_query_cache ORDER BY created_at ASC LIMIT ?
           )`
        )
        .run(count - MAX_ROWS);
    }
  } finally {
    if (ownsDb) db.close();
  }
}

/**
 * クエリを埋め込んでキャッシュに保存する (非同期・多重発火抑止つき)。
 * 失敗は throw せず warn に流す (RAG は議論を止めない方針。埋め込み系の不調は
 * ここで観測できるようログには必ず出す)。
 * @implements SPEC-VOICE-RAG-HYBRID-QUERY-CACHE
 */
export function warmQueryEmbedding(
  raw: RawDb,
  embedder: EmbeddingClient,
  model: string,
  queryText: string,
  warn: (msg: string) => void
): Promise<void> {
  const key = hashQuery(model, queryText);
  const databasePath = raw.name;
  if (memory.has(key)) return Promise.resolve();
  const running = inflight.get(key);
  if (running) return running;

  const p = (async () => {
    const [vec] = await embedder.embed([queryText]);
    if (!isEmbeddingVector(vec)) throw new Error("埋め込み応答が有効な有限数ベクトルではない");
    memory.set(key, vec);
    persistQueryVector(raw, databasePath, key, vec);
  })()
    .catch((e) => {
      warn(`クエリ埋め込みの温めに失敗 (キーワード検索で継続): ${(e as Error).message}`);
    })
    .finally(() => {
      inflight.delete(key);
    });
  inflight.set(key, p);
  return p;
}

/** テスト用: プロセス内キャッシュを空にする。 */
export function _resetQueryEmbedCache(): void {
  memory.clear();
  inflight.clear();
}
