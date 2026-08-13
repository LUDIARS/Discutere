/**
 * 埋め込みリクエストのリトライ判定と再試行 (spec/feature/voice-rag-hybrid.md)。
 *
 * 実運用 (420k 件・十数時間のバッチ構築) で、Ollama の一過性遅延が 1 バッチの
 * タイムアウトを超えると全体が即死する事象が実際に発生した。ここは
 *   - 一過性 (タイムアウト / ネットワーク断 / 5xx / 429) → 指数バックオフで再試行
 *   - 恒常 (4xx / 応答形式不正) → 即 throw (fail-fast、リトライで隠さない)
 * を分ける層。リトライ発生は warn で必ず観測可能にする (silent retry 禁止)。
 *
 * 純ロジック (sleep 注入可) — 単体テスト可能。
 */

/** HTTP ステータスを保持する埋め込みエラー (リトライ判定に使う)。 */
export class EmbeddingHttpError extends Error {
  /** @implements SPEC-VOICE-RAG-HYBRID-CONFIG */
  constructor(
    public readonly status: number,
    statusText: string
  ) {
    // 応答本文は入力テキストを含み得るため message に載せない。
    super(`embedding HTTP ${status} ${statusText}`.trim());
    this.name = "EmbeddingHttpError";
  }
}

/**
 * 一過性 (再試行に値する) エラーか。
 * @implements SPEC-VOICE-RAG-HYBRID-CONFIG
 */
export function isRetryableEmbeddingError(err: unknown): boolean {
  if (err instanceof EmbeddingHttpError) {
    return err.status >= 500 || err.status === 429;
  }
  if (err instanceof Error) {
    // fetch の AbortController タイムアウトは DOMException name=AbortError。
    if (err.name === "AbortError" || err.name === "TimeoutError") return true;
    // undici のネットワーク断は TypeError ("fetch failed") に包まれる。
    if (err.name === "TypeError") return true;
  }
  return false;
}

export interface EmbedRetryOptions {
  /** 総試行回数 (1 = リトライ無し)。 */
  attempts: number;
  /** 初回リトライ前の待ち ms。以降は 2 倍ずつ (attempt 指数バックオフ)。 */
  baseDelayMs: number;
}

export const DEFAULT_EMBED_RETRY: EmbedRetryOptions = { attempts: 3, baseDelayMs: 2_000 };

const defaultSleep = (ms: number): Promise<void> => new Promise((r) => setTimeout(r, ms));

/**
 * 一過性エラーに限り指数バックオフで再試行する。
 * 恒常エラーは即 throw、試行し尽くしたら最後のエラーを throw する。
 * @implements SPEC-VOICE-RAG-HYBRID-CONFIG
 */
export async function withEmbedRetry<T>(
  run: () => Promise<T>,
  options: EmbedRetryOptions = DEFAULT_EMBED_RETRY,
  warn: (msg: string) => void = (m) => console.warn(`[embed-retry] ${m}`),
  sleep: (ms: number) => Promise<void> = defaultSleep
): Promise<T> {
  if (!Number.isFinite(options.attempts) || !Number.isInteger(options.attempts) || options.attempts < 1) {
    throw new Error("embedding retry attempts must be a positive integer");
  }
  if (!Number.isFinite(options.baseDelayMs) || options.baseDelayMs < 0) {
    throw new Error("embedding retry baseDelayMs must be a non-negative finite number");
  }
  const attempts = options.attempts;
  let lastError: unknown;
  for (let attempt = 1; attempt <= attempts; attempt += 1) {
    try {
      return await run();
    } catch (err) {
      lastError = err;
      if (!isRetryableEmbeddingError(err) || attempt === attempts) throw err;
      const delay = options.baseDelayMs * 2 ** (attempt - 1);
      warn(
        `一過性の埋め込み失敗 (${(err as Error).message}) — ${delay}ms 待って再試行 (${attempt}/${attempts - 1})`
      );
      await sleep(delay);
    }
  }
  // attempts >= 1 なので到達しないが、型のため。
  throw lastError;
}
