/**
 * Anthropic SDK 実装 (Phase 0)。
 *
 * 切り出し時は dependencies に @anthropic-ai/sdk を入れる。 Discutere main の
 * dependencies に追加してあるが、 package 切り出し時にも忘れず移植する。
 *
 * - env ANTHROPIC_API_KEY を読む (空文字なら invoke は ok:false を返す)
 * - default model: claude-haiku-4-5-20251001 (議論の細かい応答は haiku で十分速い)
 * - prompt caching は Phase 1 で導入予定 (claude-api skill 参照)
 */

import type { LLMClient, LLMInvokeArgs, LLMResult } from "./client.js";
import { OAUTH_BETA_HEADER } from "./claude-code-auth.js";
import { logLlm } from "./llm-vg.js";

const DEFAULT_MODEL = "claude-haiku-4-5-20251001";
const DEFAULT_MAX_TOKENS = 1024;
const DEFAULT_TIMEOUT_MS = 60_000;

interface AnthropicMessageContent {
  type: string;
  text?: string;
}

interface AnthropicMessageResponse {
  content?: AnthropicMessageContent[];
  usage?: {
    input_tokens?: number;
    output_tokens?: number;
    cache_read_input_tokens?: number;
    cache_creation_input_tokens?: number;
  };
}

export interface AnthropicSdkClientOptions {
  /** 従量 API キー (x-api-key)。 */
  apiKey?: string;
  /**
   * サブスク OAuth トークンを動的取得する関数 (E)。返り値が非 null なら
   * Authorization: Bearer + anthropic-beta: oauth-2025-04-20 で叩く (apiKey より優先)。
   * Claude Code 認証は readClaudeCodeToken を渡す。
   */
  getAuthToken?: () => string | null;
  defaultModel?: string;
  /** system ブロックに cache_control を付与してプロンプトキャッシュを効かせる (既定 true)。 */
  enableCache?: boolean;
}

export class AnthropicSdkClient implements LLMClient {
  private readonly apiKey: string;
  private readonly getAuthToken?: () => string | null;
  private readonly defaultModel: string;
  private readonly enableCache: boolean;

  constructor(options: AnthropicSdkClientOptions = {}) {
    this.apiKey = options.apiKey ?? process.env.ANTHROPIC_API_KEY ?? "";
    this.getAuthToken = options.getAuthToken;
    this.defaultModel = options.defaultModel ?? DEFAULT_MODEL;
    this.enableCache = options.enableCache ?? true;
  }

  async invoke(args: LLMInvokeArgs): Promise<LLMResult> {
    // 認証: OAuth (サブスク) を優先し、無ければ x-api-key (従量)。
    const oauthToken = this.getAuthToken?.() ?? null;
    if (!oauthToken && !this.apiKey) {
      return { ok: false, error: "no credentials (OAuth token / ANTHROPIC_API_KEY 共に無し)" };
    }

    const model = args.model ?? this.defaultModel;
    const maxTokens = args.maxTokens ?? DEFAULT_MAX_TOKENS;
    const timeoutMs = args.timeoutMs ?? DEFAULT_TIMEOUT_MS;

    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), timeoutMs);

    // 認証ヘッダ。
    const headers: Record<string, string> = {
      "content-type": "application/json",
      "anthropic-version": "2023-06-01",
    };
    if (oauthToken) {
      headers["authorization"] = `Bearer ${oauthToken}`;
      headers["anthropic-beta"] = OAUTH_BETA_HEADER;
    } else {
      headers["x-api-key"] = this.apiKey;
    }

    // system: cache_control 付きブロックにして session 不変部をキャッシュ (E)。
    const system = args.system
      ? [
          {
            type: "text",
            text: args.system,
            ...(this.enableCache ? { cache_control: { type: "ephemeral" } } : {}),
          },
        ]
      : undefined;

    try {
      const res = await fetch("https://api.anthropic.com/v1/messages", {
        method: "POST",
        headers,
        body: JSON.stringify({
          model,
          max_tokens: maxTokens,
          system,
          messages: [{ role: "user", content: args.prompt }],
        }),
        signal: controller.signal,
      });

      if (!res.ok) {
        const text = await res.text().catch(() => "");
        return {
          ok: false,
          error: `anthropic http ${res.status}: ${text.slice(0, 200)}`,
        };
      }

      const json = (await res.json()) as AnthropicMessageResponse;
      const text = (json.content ?? [])
        .filter((c) => c.type === "text" && typeof c.text === "string")
        .map((c) => c.text as string)
        .join("\n")
        .trim();

      if (text.length === 0) {
        logLlm({ backend: 'anthropic-api', model, system: args.system, prompt: args.prompt, ok: false, error: 'empty text response' });
        return { ok: false, error: "empty text response" };
      }

      logLlm({
        backend: 'anthropic-api',
        model,
        system: args.system,
        prompt: args.prompt,
        ok: true,
        input_tokens: json.usage?.input_tokens,
        output_tokens: json.usage?.output_tokens,
        cache_read_tokens: json.usage?.cache_read_input_tokens,
        cache_creation_tokens: json.usage?.cache_creation_input_tokens,
      });
      return {
        ok: true,
        text,
        usage: json.usage,
      };
    } catch (err) {
      const message = (err as Error).message ?? String(err);
      logLlm({ backend: 'anthropic-api', model, system: args.system, prompt: args.prompt, ok: false, error: `invoke failed: ${message}` });
      return { ok: false, error: `anthropic invoke failed: ${message}` };
    } finally {
      clearTimeout(timer);
    }
  }
}
