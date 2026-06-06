/**
 * 常駐ワーカー callback endpoint (内部用)。
 *
 * ワーカー (= LICTOR_DISABLE_CONCORDIA で起動した Lictor セッション) が
 * curl で叩く:
 *   - POST /internal/worker/register  { workerId, lictorPort } : 起動時の自己登録
 *   - POST /internal/worker/utterance { reqId, workerId, text } : 1 ターンの発話
 *
 * 認証ミドルウェア (userContext) より前に mount し、Discord/Cernere 認証を要求
 * しない (呼び元は loopback のワーカーのみ)。WorkerPool は index.ts で注入する。
 */

import { Hono } from "hono";

import type { WorkerPool } from "../persona-engine/worker-pool/pool.js";

let pool: WorkerPool | null = null;

/** index.ts から WorkerPool を注入 (backend=worker-pool 時のみ非 null)。 */
export function setWorkerPool(p: WorkerPool | null): void {
  pool = p;
}

const workerRoutes = new Hono();

workerRoutes.post("/worker/register", async (c) => {
  if (!pool) return c.json({ error: "worker pool not initialized" }, 503);
  const body = (await c.req.json().catch(() => null)) as
    | { workerId?: unknown; lictorPort?: unknown }
    | null;
  if (!body || typeof body.workerId !== "string" || typeof body.lictorPort !== "number") {
    return c.json({ error: "workerId (string) and lictorPort (number) required" }, 400);
  }
  const ok = pool.registerPort(body.workerId, body.lictorPort);
  return c.json({ ok });
});

workerRoutes.post("/worker/utterance", async (c) => {
  if (!pool) return c.json({ error: "worker pool not initialized" }, 503);
  const body = (await c.req.json().catch(() => null)) as
    | { reqId?: unknown; workerId?: unknown; text?: unknown }
    | null;
  if (!body || typeof body.reqId !== "string") {
    return c.json({ error: "reqId (string) required" }, 400);
  }
  pool.onUtterance(
    body.reqId,
    typeof body.workerId === "string" ? body.workerId : "",
    typeof body.text === "string" ? body.text : ""
  );
  return c.json({ ok: true });
});

export { workerRoutes };
