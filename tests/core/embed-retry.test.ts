import assert from "node:assert/strict";

import {
  EmbeddingHttpError,
  isRetryableEmbeddingError,
  withEmbedRetry,
} from "../../src/core/vectors/embed-retry.js";

const noSleep = async (): Promise<void> => {};

// ── isRetryableEmbeddingError: 一過性/恒常の分類 ─────────────────────────────
{
  assert.equal(isRetryableEmbeddingError(new EmbeddingHttpError(500, "Internal")), true);
  assert.equal(isRetryableEmbeddingError(new EmbeddingHttpError(429, "Too Many")), true);
  assert.equal(isRetryableEmbeddingError(new EmbeddingHttpError(400, "Bad Request")), false);
  assert.equal(isRetryableEmbeddingError(new EmbeddingHttpError(404, "Not Found")), false);

  const abort = new Error("This operation was aborted");
  abort.name = "AbortError";
  assert.equal(isRetryableEmbeddingError(abort), true, "タイムアウト (AbortError) は一過性");

  const network = new TypeError("fetch failed");
  assert.equal(isRetryableEmbeddingError(network), true, "ネットワーク断 (TypeError) は一過性");

  assert.equal(isRetryableEmbeddingError(new Error("応答件数不一致")), false, "形式不正は恒常");
  assert.equal(isRetryableEmbeddingError("string"), false);
}
console.log("ok isRetryableEmbeddingError");

// ── withEmbedRetry: 一過性は再試行して成功に至る ─────────────────────────────
{
  let calls = 0;
  const warns: string[] = [];
  const result = await withEmbedRetry(
    async () => {
      calls += 1;
      if (calls < 3) {
        const e = new Error("timeout");
        e.name = "AbortError";
        throw e;
      }
      return "ok";
    },
    { attempts: 3, baseDelayMs: 1 },
    (m) => warns.push(m),
    noSleep
  );
  assert.equal(result, "ok");
  assert.equal(calls, 3);
  assert.equal(warns.length, 2, "リトライは warn で観測できる (silent retry 禁止)");
}
console.log("ok withEmbedRetry transient recovery");

// ── withEmbedRetry: 恒常エラーは即 throw (リトライしない) ────────────────────
{
  let calls = 0;
  await assert.rejects(
    withEmbedRetry(
      async () => {
        calls += 1;
        throw new EmbeddingHttpError(400, "Bad Request");
      },
      { attempts: 3, baseDelayMs: 1 },
      () => {},
      noSleep
    ),
    (e: Error) => e instanceof EmbeddingHttpError && e.status === 400
  );
  assert.equal(calls, 1, "恒常エラーで再試行しない");
}
console.log("ok withEmbedRetry permanent fail-fast");

// ── withEmbedRetry: 試行し尽くしたら最後のエラーを投げる ─────────────────────
{
  let calls = 0;
  await assert.rejects(
    withEmbedRetry(
      async () => {
        calls += 1;
        throw new EmbeddingHttpError(503, "Unavailable");
      },
      { attempts: 3, baseDelayMs: 1 },
      () => {},
      noSleep
    ),
    (e: Error) => e instanceof EmbeddingHttpError && e.status === 503
  );
  assert.equal(calls, 3);
}
console.log("ok withEmbedRetry exhaustion");

// ── withEmbedRetry: 不正な設定は即 throw (無限リトライを防ぐ) ────────────────
{
  await assert.rejects(
    withEmbedRetry(async () => "ok", { attempts: Infinity, baseDelayMs: 1 }, () => {}, noSleep),
    /attempts must be a positive integer/
  );
  await assert.rejects(
    withEmbedRetry(async () => "ok", { attempts: 1, baseDelayMs: Number.NaN }, () => {}, noSleep),
    /baseDelayMs must be a non-negative finite number/
  );
}
console.log("ok withEmbedRetry option validation");

console.log("embed-retry tests: all passed");
