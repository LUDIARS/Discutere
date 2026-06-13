/**
 * LLM 呼び出しコスト + transcript ロガー (OVERVIEW §8, T1 spec §4)。
 *
 * LLMClient をラップする高階関数 withCostLog を提供する。
 * 呼び出しごとに llm_call_log へ 1 行書き込む。
 * トークン数を返さない backend (claude-cli / worker-pool) は null 許容 (best-effort)。
 */

import type { LLMClient, LLMInvokeArgs, LLMResult } from "../persona-engine/llm/client.js";
import { getFlowDb } from "./db/connection.js";

export interface CostLogContext {
  flow: string;
  sessionId: string;
  round?: number;
  turn?: number;
  role?: string;
  persona?: string;
  location: string;
}

/**
 * LLMClient をラップし、呼び出しごとに llm_call_log へ記録する高階関数。
 * 元の LLMClient の挙動は変えない。
 */
export function withCostLog(client: LLMClient, ctx: CostLogContext): LLMClient {
  return {
    async invoke(args: LLMInvokeArgs): Promise<LLMResult> {
      const start = Date.now();
      const result = await client.invoke(args);
      const latencyMs = Date.now() - start;

      const db = getFlowDb();
      const usage = result.ok ? result.usage : undefined;
      db.prepare(
        `INSERT INTO llm_call_log
          (flow, session_id, round, turn, role, persona, location, model, backend, latency_ms, input_tokens, output_tokens, prompt, response, created_at)
         VALUES
          (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`
      ).run(
        ctx.flow,
        ctx.sessionId,
        ctx.round ?? null,
        ctx.turn ?? null,
        ctx.role ?? null,
        ctx.persona ?? null,
        ctx.location,
        args.model ?? null,
        null, // backend は呼び出し元の設定から渡すことが多いが LLMClient 境界では不明のため null
        latencyMs,
        usage?.input_tokens ?? null,
        usage?.output_tokens ?? null,
        args.prompt,
        result.ok ? result.text : null,
        Date.now()
      );

      return result;
    },
  };
}
