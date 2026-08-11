import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";

import Database from "better-sqlite3";

import { loadConfig } from "../../src/config.js";
import { createOpenAiCompatEmbedder, type EmbeddingConfig } from "../../src/core/vectors/embedder.js";
import {
  _resetQueryEmbedCache,
  getCachedQueryVector,
  warmQueryEmbedding,
} from "../../src/core/vectors/query-embed-cache.js";

const validConfig: EmbeddingConfig = {
  enabled: true,
  baseUrl: "http://127.0.0.1:11434/v1",
  model: "test-model",
  timeoutMs: 1_000,
  batchSize: 2,
};

for (const batchSize of [0, -1, 1.5]) {
  process.env.DISCUTERE_EMBEDDING_BATCH_SIZE = String(batchSize);
  assert.throws(
    () => loadConfig(),
    /embedding\.batchSize must be a positive integer/,
    `config も batchSize=${batchSize} を拒否する`
  );
  assert.throws(
    () => createOpenAiCompatEmbedder({ ...validConfig, batchSize }),
    /batchSize must be a positive integer/,
    `batchSize=${batchSize} を拒否する`
  );
}
delete process.env.DISCUTERE_EMBEDDING_BATCH_SIZE;
assert.throws(
  () => createOpenAiCompatEmbedder({ ...validConfig, baseUrl: "file:///tmp/embeddings" }),
  /HTTP\(S\)/,
  "HTTP(S) 以外の埋め込み endpoint を拒否する"
);
console.log("ok embedding config validation");

// 同期検索の直後に所有者が DB を閉じても、非同期 warm 完了時に同じ DB へ永続化できる。
{
  const workDir = path.resolve(".tmp/query-embed-cache");
  fs.rmSync(workDir, { recursive: true, force: true });
  fs.mkdirSync(workDir, { recursive: true });
  const dbPath = path.join(workDir, "cache.sqlite");
  const db = new Database(dbPath);
  const warnings: string[] = [];
  const warming = warmQueryEmbedding(
    db,
    {
      embed: async () => {
        await Promise.resolve();
        return [[0.25, 0.75]];
      },
    },
    "test-model",
    "秘密の議題",
    (message) => warnings.push(message)
  );
  db.close();
  await warming;

  _resetQueryEmbedCache();
  const reopened = new Database(dbPath);
  assert.deepEqual(
    getCachedQueryVector(reopened, "test-model", "秘密の議題"),
    [0.25, 0.75],
    "閉じた呼び出し元 DB を再利用せず永続キャッシュを読める"
  );
  const row = reopened
    .prepare("SELECT query_text FROM embedding_query_cache")
    .get() as { query_text: string };
  assert.equal(row.query_text, "", "議題本文を永続キャッシュへ保存しない");
  assert.deepEqual(warnings, [], "DB close を埋め込み障害として警告しない");
  reopened.close();
  fs.rmSync(workDir, { recursive: true, force: true });
}
console.log("ok query embedding cache survives owner close");
