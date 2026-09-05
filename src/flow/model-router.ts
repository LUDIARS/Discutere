/**
 * ModelRouterLlm — model spec の provider で LLMClient を振り分ける。
 *
 * 議論フローは 1 つの `LLMClient` に全ペルソナの発話を流すが、ペルソナごとの
 * model が Claude 系 (`claude-*`) と Codex 系 (`gpt-*`) で混在しうる
 * (spec/feature/flow/model-roster.md)。本ラッパが `gpt-` 接頭辞を Codex 経路へ、
 * それ以外を既存チェーン (worker-pool → SDK → claude -p) へ回す。
 * Codex 経路が失敗した場合は既存チェーンへフォールバックしない
 * (Claude で代弁させると「GPT の意見」が偽装されるため、エラーとして表面化させる)。
 */

import type { LLMClient, LLMInvokeArgs, LLMResult } from "../persona-engine/llm/client.js";
import { isCodexModel } from "../persona-engine/llm/model-spec.js";

export class ModelRouterLlm implements LLMClient {
  /** @implements SPEC-FLOW-MODEL-ROSTER — provider ごとのクライアントを固定する。 */
  constructor(
    private readonly claude: LLMClient,
    private readonly codex: LLMClient | null
  ) {}

  /** @implements SPEC-FLOW-MODEL-ROSTER — gpt-* だけを Codex 経路へ送る。 */
  invoke(args: LLMInvokeArgs): Promise<LLMResult> {
    if (isCodexModel(args.model)) {
      if (!this.codex) {
        return Promise.resolve({ ok: false, error: "model-router: codex 経路なし" });
      }
      return this.codex.invoke(args);
    }
    return this.claude.invoke(args);
  }
}
