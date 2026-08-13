/**
 * 埋め込みクライアント — OpenAI 互換 `/embeddings` を叩く (Ollama / vLLM / LM Studio)。
 *
 * 外部の声 RAG の意味検索用。既定は Ollama ローカル (`http://localhost:11434/v1`) の
 * bge-m3。GPU が使えない環境 (PTX 不一致等) でも bge-m3 の埋め込みは CPU で実用速度。
 *
 * llm.local (chat) とは責務が別 (埋め込み専用モデル/エンドポイント) なので
 * config も `embedding` セクションとして独立させる。
 */

import { isEmbeddingVector } from "./vector-math.js";
import {
  DEFAULT_EMBED_RETRY,
  EmbeddingHttpError,
  withEmbedRetry,
  type EmbedRetryOptions,
} from "./embed-retry.js";

export interface EmbeddingConfig {
  /** 意味検索 (ハイブリッド RAG) を有効にするか。false なら従来のキーワード検索のみ。 */
  enabled: boolean;
  /** OpenAI 互換ベース URL (末尾 `/v1`)。 */
  baseUrl: string;
  /** 埋め込みモデル名 (例 `bge-m3`)。 */
  model: string;
  /** 任意。設定時のみ Bearer 認証。 */
  apiKey?: string;
  /** 応答待ちタイムアウト ms。 */
  timeoutMs: number;
  /** 一括埋め込み時の 1 リクエスト最大テキスト数。 */
  batchSize: number;
}

export interface EmbeddingClient {
  /** テキスト群をベクトル化する。入力順を保つ。失敗は throw (呼び出し側で degrade を判断)。 */
  embed(texts: readonly string[]): Promise<number[][]>;
}

interface OpenAiEmbeddingResponse {
  data?: Array<{ index?: number; embedding?: number[] }>;
}

/**
 * OpenAI 互換 `/embeddings` クライアントを作る。
 * 一過性の失敗 (タイムアウト / ネットワーク断 / 5xx / 429) はバッチ単位で
 * 指数バックオフ再試行する (embed-retry.ts)。恒常エラーは即 throw。
 * @implements SPEC-VOICE-RAG-HYBRID-CONFIG
 */
export function createOpenAiCompatEmbedder(
  config: EmbeddingConfig,
  hooks: { warn?: (msg: string) => void; retry?: EmbedRetryOptions } = {}
): EmbeddingClient {
  const warn = hooks.warn ?? ((m: string) => console.warn(`[embedder] ${m}`));
  const retry = hooks.retry ?? DEFAULT_EMBED_RETRY;
  if (!Number.isInteger(config.batchSize) || config.batchSize <= 0) {
    throw new Error("embedding.batchSize must be a positive integer");
  }
  if (!Number.isFinite(config.timeoutMs) || config.timeoutMs <= 0) {
    throw new Error("embedding.timeoutMs must be a positive number");
  }
  if (config.model.trim().length === 0) {
    throw new Error("embedding.model must not be empty");
  }
  let endpoint: URL;
  try {
    endpoint = new URL(`${config.baseUrl.replace(/\/+$/, "")}/embeddings`);
  } catch {
    throw new Error("embedding.baseUrl must be a valid HTTP(S) URL");
  }
  if (!/^https?:$/.test(endpoint.protocol) || endpoint.username || endpoint.password) {
    throw new Error("embedding.baseUrl must be an HTTP(S) URL without embedded credentials");
  }
  if (endpoint.search || endpoint.hash) {
    throw new Error("embedding.baseUrl must not contain a query string or fragment");
  }
  const url = endpoint.toString();
  const headers: Record<string, string> = { "content-type": "application/json" };
  if (config.apiKey) headers.authorization = `Bearer ${config.apiKey}`;

  async function embedBatch(texts: readonly string[]): Promise<number[][]> {
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), config.timeoutMs);
    try {
      const res = await fetch(url, {
        method: "POST",
        headers,
        body: JSON.stringify({ model: config.model, input: texts }),
        signal: controller.signal,
        redirect: "error",
      });
      if (!res.ok) {
        // 応答本文は入力テキストや上流設定を含み得るためログへ伝播させない。
        throw new EmbeddingHttpError(res.status, res.statusText);
      }
      const json = (await res.json()) as OpenAiEmbeddingResponse;
      const data = json.data ?? [];
      if (data.length !== texts.length) {
        throw new Error(`embedding 応答件数不一致: 入力 ${texts.length} 件 / 応答 ${data.length} 件`);
      }
      // index フィールドで入力順に整列 (OpenAI 互換実装は順序保証が曖昧なため明示)。
      const out: Array<number[] | undefined> = new Array(texts.length);
      let dimension: number | null = null;
      data.forEach((d, i) => {
        const idx = typeof d.index === "number" ? d.index : i;
        if (!Number.isInteger(idx) || idx < 0 || idx >= texts.length || out[idx]) {
          throw new Error(`embedding 応答の index が不正または重複している (index=${idx})`);
        }
        if (!isEmbeddingVector(d.embedding)) {
          throw new Error(`embedding 応答に有効なベクトルが無い (index=${idx})`);
        }
        if (dimension !== null && d.embedding.length !== dimension) {
          throw new Error("embedding 応答のベクトル次元が一致しない");
        }
        dimension = d.embedding.length;
        out[idx] = d.embedding;
      });
      return out.map((vector, idx) => {
        if (!vector) throw new Error(`embedding 応答に index=${idx} が無い`);
        return vector;
      });
    } finally {
      clearTimeout(timer);
    }
  }

  return {
    async embed(texts: readonly string[]): Promise<number[][]> {
      if (texts.length === 0) return [];
      const out: number[][] = [];
      for (let i = 0; i < texts.length; i += config.batchSize) {
        const batch = texts.slice(i, i + config.batchSize);
        out.push(...(await withEmbedRetry(() => embedBatch(batch), retry, warn)));
      }
      return out;
    },
  };
}
