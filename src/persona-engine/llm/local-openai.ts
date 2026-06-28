/**
 * ローカル LLM (OpenAI 互換 `/v1/chat/completions`) 実装。
 *
 * 将来ローカル LLM (例 Gemma 12B) で運用するための backend。Ollama / vLLM /
 * LM Studio / llama.cpp server はいずれも OpenAI 互換の chat/completions を
 * 公開するので、 baseUrl + model を差すだけでランタイム非依存に繋がる。
 *
 * - baseUrl 例: Ollama `http://localhost:11434/v1` / vLLM `http://localhost:8000/v1`
 * - apiKey は任意 (Ollama は不要、 vLLM 等は Bearer)。
 * - SDK 非依存 (raw fetch)。 anthropic.ts と同じ思想。
 */

import type { TokenUsage } from "../types.js";
import type { LLMClient, LLMInvokeArgs, LLMResult } from "./client.js";
import { logLlm } from "./llm-vg.js";

// ローカルは課金が無いので余裕を持たせる。 特に Gemma 4 等の **reasoning モデル**は
// 思考 (応答の message.reasoning) でトークンを消費するため、 max_tokens が小さいと
// 最終回答 (message.content) が空になり ok:false になる (実機: 128 だと空 / 512 で回答)。
// 既定を広く取り空応答を防ぐ。 reasoning を持たないモデルは end_turn で早く止まるので無害。
const DEFAULT_MAX_TOKENS = 4096;
const DEFAULT_TIMEOUT_MS = 120_000;

interface OpenAiChatChoice {
  message?: { content?: string };
}

interface OpenAiChatResponse {
  choices?: OpenAiChatChoice[];
  usage?: { prompt_tokens?: number; completion_tokens?: number };
}

export interface LocalOpenAiClientOptions {
  /** OpenAI 互換エンドポイントのベース URL (末尾 `/v1` まで、 例 `http://localhost:11434/v1`)。 */
  baseUrl: string;
  /** 既定モデル (例 `gemma4:12b`)。 invoke args.model が優先。 */
  defaultModel?: string;
  /** 任意。 設定時のみ Authorization: Bearer を送る。 */
  apiKey?: string;
  defaultTimeoutMs?: number;
  /** テスト用 fetch 差し替え。 */
  fetchImpl?: typeof fetch;
}

/** OpenAI usage → Discutere の TokenUsage にマップ。 */
function mapUsage(usage: OpenAiChatResponse["usage"]): TokenUsage | undefined {
  if (!usage) return undefined;
  return { input_tokens: usage.prompt_tokens, output_tokens: usage.completion_tokens };
}

export class LocalOpenAiClient implements LLMClient {
  private readonly baseUrl: string;
  private readonly defaultModel: string;
  private readonly apiKey: string;
  private readonly defaultTimeoutMs: number;
  private readonly doFetch: typeof fetch;

  constructor(options: LocalOpenAiClientOptions) {
    // 末尾スラッシュを正規化 (`/v1/` でも `/v1` でも可)。
    this.baseUrl = (options.baseUrl ?? "").replace(/\/+$/, "");
    this.defaultModel = options.defaultModel ?? "";
    this.apiKey = options.apiKey ?? "";
    this.defaultTimeoutMs = options.defaultTimeoutMs ?? DEFAULT_TIMEOUT_MS;
    this.doFetch = options.fetchImpl ?? fetch;
  }

  async invoke(args: LLMInvokeArgs): Promise<LLMResult> {
    if (!this.baseUrl) return { ok: false, error: "local LLM baseUrl not set" };
    const model = args.model ?? this.defaultModel;
    if (!model) return { ok: false, error: "local LLM model not set" };

    const maxTokens = args.maxTokens ?? DEFAULT_MAX_TOKENS;
    const timeoutMs = args.timeoutMs ?? this.defaultTimeoutMs;

    const messages: Array<{ role: string; content: string }> = [];
    if (args.system) messages.push({ role: "system", content: args.system });
    messages.push({ role: "user", content: args.prompt });

    const headers: Record<string, string> = { "content-type": "application/json" };
    if (this.apiKey) headers.authorization = `Bearer ${this.apiKey}`;

    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), timeoutMs);
    try {
      const res = await this.doFetch(`${this.baseUrl}/chat/completions`, {
        method: "POST",
        headers,
        body: JSON.stringify({ model, max_tokens: maxTokens, messages, stream: false }),
        signal: controller.signal,
      });
      if (!res.ok) {
        const body = await res.text().catch(() => "");
        return { ok: false, error: `local LLM http ${res.status}: ${body.slice(0, 200)}` };
      }
      const json = (await res.json()) as OpenAiChatResponse;
      const text = (json.choices?.[0]?.message?.content ?? "").trim();
      if (text.length === 0) {
        logLlm({ backend: 'local-openai', model, system: args.system, prompt: args.prompt, ok: false, error: 'empty text response' });
        return { ok: false, error: "local LLM empty text response" };
      }
      const usage = mapUsage(json.usage);
      logLlm({
        backend: 'local-openai',
        model,
        system: args.system,
        prompt: args.prompt,
        ok: true,
        input_tokens: usage?.input_tokens,
        output_tokens: usage?.output_tokens,
      });
      return { ok: true, text, usage };
    } catch (err) {
      const errMsg = (err as Error).message ?? String(err);
      logLlm({ backend: 'local-openai', model, system: args.system, prompt: args.prompt, ok: false, error: `invoke failed: ${errMsg}` });
      return { ok: false, error: `local LLM invoke failed: ${errMsg}` };
    } finally {
      clearTimeout(timer);
    }
  }
}
