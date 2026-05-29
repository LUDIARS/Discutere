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

const DEFAULT_MODEL = "claude-haiku-4-5-20251001";
const DEFAULT_MAX_TOKENS = 1024;
const DEFAULT_TIMEOUT_MS = 60_000;

interface AnthropicMessageContent {
  type: string;
  text?: string;
}

interface AnthropicMessageResponse {
  content?: AnthropicMessageContent[];
  usage?: { input_tokens?: number; output_tokens?: number };
}

export interface AnthropicSdkClientOptions {
  apiKey?: string;
  defaultModel?: string;
}

export class AnthropicSdkClient implements LLMClient {
  private readonly apiKey: string;
  private readonly defaultModel: string;

  constructor(options: AnthropicSdkClientOptions = {}) {
    this.apiKey = options.apiKey ?? process.env.ANTHROPIC_API_KEY ?? "";
    this.defaultModel = options.defaultModel ?? DEFAULT_MODEL;
  }

  async invoke(args: LLMInvokeArgs): Promise<LLMResult> {
    if (!this.apiKey) {
      return { ok: false, error: "ANTHROPIC_API_KEY not set" };
    }

    const model = args.model ?? this.defaultModel;
    const maxTokens = args.maxTokens ?? DEFAULT_MAX_TOKENS;
    const timeoutMs = args.timeoutMs ?? DEFAULT_TIMEOUT_MS;

    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), timeoutMs);

    try {
      const res = await fetch("https://api.anthropic.com/v1/messages", {
        method: "POST",
        headers: {
          "content-type": "application/json",
          "x-api-key": this.apiKey,
          "anthropic-version": "2023-06-01",
        },
        body: JSON.stringify({
          model,
          max_tokens: maxTokens,
          system: args.system,
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
        return { ok: false, error: "empty text response" };
      }

      return {
        ok: true,
        text,
        usage: json.usage,
      };
    } catch (err) {
      const message = (err as Error).message ?? String(err);
      return { ok: false, error: `anthropic invoke failed: ${message}` };
    } finally {
      clearTimeout(timer);
    }
  }
}
