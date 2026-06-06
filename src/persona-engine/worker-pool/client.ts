/**
 * WorkerPoolClient — LLMClient 実装。
 *
 * persona-engine の発話生成を「常駐ワーカー (サブスク Lictor セッション)」に
 * 委ねる。invoke({personaId}) で personaId に対応するワーカーへターンを投げ、
 * 発話 callback が戻るまで待って text を返す。
 *
 * personaId が未指定 / 未登録ワーカーの場合は ok:false を返し、engine 側は
 * skip 扱いにする (= 後方互換: 既存 backend は personaId を無視する)。
 */

import { randomUUID } from "node:crypto";

import type { LLMClient, LLMInvokeArgs, LLMResult } from "../llm/client.js";
import type { WorkerPool } from "./pool.js";

export class WorkerPoolClient implements LLMClient {
  constructor(private readonly pool: WorkerPool) {}

  async invoke(args: LLMInvokeArgs): Promise<LLMResult> {
    const workerId = args.personaId;
    if (!workerId) {
      return { ok: false, error: "worker-pool: personaId 未指定 (ルーティング不能)" };
    }
    if (!this.pool.hasWorker(workerId)) {
      return { ok: false, error: `worker-pool: persona '${workerId}' に対応するワーカー無し` };
    }
    if (!this.pool.isReady(workerId)) {
      return { ok: false, error: `worker-pool: worker '${workerId}' 未登録 (port 未取得)` };
    }
    const reqId = randomUUID();
    try {
      const text = await this.pool.dispatch(workerId, {
        reqId,
        system: args.system ?? "",
        prompt: args.prompt,
      });
      // engine の handler は LLM が {action,...} JSON を返す前提。ワーカーは素の
      // 発話テキストを返すので、ここで action JSON に包んで handler に渡す。
      const trimmed = text.trim();
      if (trimmed.length === 0) {
        return { ok: true, text: JSON.stringify({ action: "skip", reasoning: "worker skip (空応答)" }) };
      }
      return { ok: true, text: JSON.stringify({ action: "post_utterance", text: trimmed }) };
    } catch (err) {
      return { ok: false, error: `worker-pool: ${(err as Error).message}` };
    }
  }
}
