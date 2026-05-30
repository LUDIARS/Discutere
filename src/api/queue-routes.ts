/**
 * 議論キュー可視化 API (議論の展開 / 積まれているキューの可視化).
 *
 * GET /api/admin/queue — 進行中 session / 未処理 gap / 検証待ち仮説のスナップショットを返す。
 * dashboard (HTML) と外部監視の双方から polling される。admin role 必須。
 *
 * Discatier Core は read-only で都度 open/close (command-router と同じ短命接続パターン)。
 * persona-engine db は読み取り専用接続で発火数のみ参照 (取得失敗時は fireCount=0)。
 */

import { Hono } from "hono";
import type { Context } from "hono";
import Database from "better-sqlite3";

import { getUserId, getUserRole } from "../middleware/auth.js";
import { createCore } from "../core/index.js";
import { getConfig } from "../config.js";
import { buildQueueSnapshot } from "../queue/snapshot.js";

export const queueRoutes = new Hono();

queueRoutes.get("/admin/queue", (c) => {
  const guard = requireAdmin(c);
  if (guard) return guard;

  const config = getConfig();
  const core = createCore();
  let peDb: Database.Database | null = null;
  try {
    try {
      peDb = new Database(config.personaEngine.dbPath, { readonly: true, fileMustExist: true });
      peDb.pragma("busy_timeout = 2000");
    } catch {
      peDb = null;
    }
    const snapshot = buildQueueSnapshot(core.client.raw, peDb, config.workspace);
    return c.json(snapshot);
  } finally {
    peDb?.close();
    core.close();
  }
});

function requireAdmin(c: Context): Response | null {
  const userId = getUserId(c);
  const role = getUserRole(c);
  if (!userId) return c.json({ error: "Authentication required" }, 401);
  if (role !== "admin") return c.json({ error: "admin role required" }, 403);
  return null;
}
