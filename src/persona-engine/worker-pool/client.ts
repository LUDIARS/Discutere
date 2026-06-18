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
  /**
   * @param pool 常駐ワーカープール
   * @param fallback worker 不在/未起動時に使う既存 LLM (= ClaudeCliClient)。
   *   指定時、personaId のワーカーが起動していなければ claude -p にフォールバックし、
   *   model は worker 定義 (delegation) に沿わせる。
   */
  constructor(
    private readonly pool: WorkerPool,
    private readonly fallback?: LLMClient | null
  ) {}

  async invoke(args: LLMInvokeArgs): Promise<LLMResult> {
    const workerId = args.personaId;
    if (!workerId) {
      return { ok: false, error: "worker-pool: personaId 未指定 (ルーティング不能)" };
    }

    const ready = this.pool.hasWorker(workerId) && this.pool.isReady(workerId);
    if (ready) {
      const reqId = randomUUID();
      try {
        const { text, usage } = await this.pool.dispatch(workerId, {
          reqId,
          system: args.system ?? "",
          prompt: args.prompt,
        });
        // engine の handler は {action,...} JSON 前提。ワーカーは素の発話テキストを
        // 返すので、ここで action JSON に包む (空応答は skip)。usage は action ラップに
        // 関係なく LLMResult に載せ、withCostLog が llm_call_log に記録する (#135)。
        const trimmed = text.trim();
        if (trimmed.length === 0) {
          return { ok: true, text: JSON.stringify({ action: "skip", reasoning: "worker skip (空応答)" }), usage };
        }
        return { ok: true, text: JSON.stringify({ action: "post_utterance", text: trimmed }), usage };
      } catch (err) {
        // worker 経路で失敗 → fallback があれば claude -p に回す。
        if (this.fallback) return this.invokeFallback(args, workerId);
        return { ok: false, error: `worker-pool: ${(err as Error).message}` };
      }
    }

    // worker 不在/未起動 → 既存どおり claude -p で動作 (model は delegation 準拠)。
    if (this.fallback) return this.invokeFallback(args, workerId);
    return { ok: false, error: `worker-pool: worker '${workerId}' 未起動 かつ fallback 無し` };
  }

  /** claude -p フォールバック。model を worker 定義に沿わせて呼ぶ。 */
  private invokeFallback(args: LLMInvokeArgs, workerId: string): Promise<LLMResult> {
    const cfg = this.pool.getWorkerConfig(workerId);
    const rawModel = cfg?.model ?? args.model;
    // fallback は claude -p。worker が GPT 等の非 claude モデル (provider: codex)
    // の場合、その model id を `claude --model <gpt-...>` に渡すと CLI が exit 1 で
    // 落ちる。非 claude モデルは undefined にして claude の既定モデルで発話させる
    // (= ペルソナの声色は traits/speech_style で保たれる。真の GPT 経路は worker 起動時)。
    const model = rawModel && /claude/i.test(rawModel) ? rawModel : undefined;
    return this.fallback!.invoke({ ...args, model });
  }
}
