/**
 * LLMClient interface — persona-engine が LLM を呼ぶ唯一の境界。
 *
 * 実装は consumer が差し替え可能 (Anthropic SDK / Codex CLI / Mock 等)。
 * Phase 0 では AnthropicSdkClient + MockLLMClient を提供。
 */

import type { TokenUsage } from "../types.js";

export interface LLMInvokeArgs {
  prompt: string;
  /** persona の speech_style など、 system message に乗せる固定指示 */
  system?: string;
  model?: string;
  maxTokens?: number;
  /** タイムアウト ms (default 60_000) */
  timeoutMs?: number;
}

export type LLMResult =
  | { ok: true; text: string; usage?: TokenUsage }
  | { ok: false; error: string };

export interface LLMClient {
  invoke(args: LLMInvokeArgs): Promise<LLMResult>;
}
