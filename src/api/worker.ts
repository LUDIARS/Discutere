/**
 * 常駐ワーカー callback endpoint (内部用)。
 *
 * ワーカー (= LICTOR_DISABLE_CONCORDIA で起動した Lictor セッション) が
 * worker-home の node スクリプト (scripts/{register,send}.mjs) から叩く
 * (旧: 生 curl。 auto-mode 分類器の遮断回避のため node 経由に置換):
 *   - POST /internal/worker/register  { workerId, lictorPort } : 起動時の自己登録
 *   - POST /internal/worker/utterance { reqId, workerId, text, usage? } : 1 ターンの発話
 *     (usage は worker-home の send.mjs が transcript から拾った token 集計。任意, #135)
 *
 * 認証ミドルウェア (userContext) より前に mount し、Discord/Cernere 認証を要求
 * しない (呼び元は loopback のワーカーのみ)。WorkerPool は index.ts で注入する。
 */

import { Hono } from "hono";

import type { TokenUsage } from "../persona-engine/types.js";
import type { WorkerPool } from "../persona-engine/worker-pool/pool.js";

/**
 * callback body の `usage` (send.mjs が transcript から拾った token) を既知の数値
 * フィールドだけに絞って TokenUsage 化する (#135)。未知/非数値は捨てる。cost_usd は
 * transcript 経由では来ない (token のみ) が、来た場合も拾えるようにする。
 */
function pickUsage(raw: unknown): TokenUsage | undefined {
  if (!raw || typeof raw !== "object") return undefined;
  const r = raw as Record<string, unknown>;
  const num = (v: unknown): number | undefined =>
    typeof v === "number" && Number.isFinite(v) ? v : undefined;
  const u: TokenUsage = {
    input_tokens: num(r.input_tokens),
    output_tokens: num(r.output_tokens),
    cache_read_input_tokens: num(r.cache_read_input_tokens),
    cache_creation_input_tokens: num(r.cache_creation_input_tokens),
  };
  const cost = num(r.cost_usd);
  if (cost !== undefined) u.cost_usd = cost;
  const hasAny =
    u.input_tokens !== undefined ||
    u.output_tokens !== undefined ||
    u.cache_read_input_tokens !== undefined ||
    u.cache_creation_input_tokens !== undefined;
  return hasAny ? u : undefined;
}

let pool: WorkerPool | null = null;

/** index.ts から WorkerPool を注入 (backend=worker-pool 時のみ非 null)。 */
export function setWorkerPool(p: WorkerPool | null): void {
  pool = p;
}

const workerRoutes = new Hono();

workerRoutes.post("/worker/register", async (c) => {
  if (!pool) return c.json({ error: "worker pool not initialized" }, 503);
  const body = (await c.req.json().catch(() => null)) as
    | { workerId?: unknown; lictorPort?: unknown; lictorPid?: unknown }
    | null;
  if (!body || typeof body.workerId !== "string" || typeof body.lictorPort !== "number") {
    return c.json({ error: "workerId (string) and lictorPort (number) required" }, 400);
  }
  const lictorPid = typeof body.lictorPid === "number" && body.lictorPid > 0 ? body.lictorPid : undefined;
  const ok = pool.registerPort(body.workerId, body.lictorPort, lictorPid);
  return c.json({ ok });
});

workerRoutes.post("/worker/utterance", async (c) => {
  if (!pool) return c.json({ error: "worker pool not initialized" }, 503);
  const body = (await c.req.json().catch(() => null)) as
    | { reqId?: unknown; workerId?: unknown; text?: unknown; usage?: unknown }
    | null;
  if (!body || typeof body.reqId !== "string") {
    return c.json({ error: "reqId (string) required" }, 400);
  }
  pool.onUtterance(
    body.reqId,
    typeof body.workerId === "string" ? body.workerId : "",
    typeof body.text === "string" ? body.text : "",
    pickUsage(body.usage)
  );
  return c.json({ ok: true });
});

export { workerRoutes };
