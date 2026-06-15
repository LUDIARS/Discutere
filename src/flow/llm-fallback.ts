/**
 * FallbackLlm — primary を試し、失敗したら fallback に回す LLMClient。
 *
 * 議論フロー (FlowDirector) は invoke に personaId を渡さない呼び出し
 * (facilitator / 投票 / 結論 / 要約 / 感情 / 分類) が多い。worker-pool backend を
 * そのまま flow に繋ぐと WorkerPoolClient が personaId 未指定で即 ok:false を
 * 返し (この経路はフォールバックしない) フロー全体が壊れる。
 *
 * 本ラッパで「worker-pool を踏襲しつつ、worker がいない / ルーティング不能な
 * 呼び出しは claude -p にフォールバックする」を flow の全 LLM 呼び出しに保証する。
 * worker が起動していて personaId が解決できる発話は worker に流れる
 * (WorkerPoolClient 内部処理)。それ以外は本ラッパが fallback (claude -p) に回す。
 */

import type { LLMClient, LLMInvokeArgs, LLMResult } from "../persona-engine/llm/client.js";

export class FallbackLlm implements LLMClient {
  constructor(
    private readonly primary: LLMClient,
    private readonly fallback: LLMClient
  ) {}

  async invoke(args: LLMInvokeArgs): Promise<LLMResult> {
    const result = await this.primary.invoke(args);
    if (result.ok) return result;
    return this.fallback.invoke(args);
  }
}
