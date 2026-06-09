/**
 * KG 共有同期の配信エンドポイント (master 側) — spec/core/KG-SYNC.md。
 *
 * GET /api/kg/migrations?since=<epochMs>[&token=<secret>]
 *   共有テーブルの shared=1 かつ updated_at>since の行を NDJSON で返す。
 *   1 行目 manifest の watermark を follower が保存し、次回 since に渡す。
 *
 * 認可: config.kgSync.serveToken が設定されている場合のみ ?token= か x-kg-token を要求する
 * (未設定なら誰でも取得可 = ローカル/Tailnet 前提)。 個人データは元々 KG に保存しない方針。
 */

import { Hono } from "hono";
import type { Context } from "hono";

import { getConfig } from "../config.js";
import { createCore } from "../core/index.js";
import { resolveActiveKgPath } from "../core/kg-registry.js";
import { exportSharedKg } from "../kg-sync/export.js";

export const kgMigrationRoutes = new Hono();

function tokenOk(c: Context): boolean {
  const required = getConfig().kgSync.serveToken;
  if (!required) return true;
  const provided = c.req.query("token") ?? c.req.header("x-kg-token");
  return provided === required;
}

kgMigrationRoutes.get("/kg/migrations", (c) => {
  if (!tokenOk(c)) return c.text("forbidden", 403);

  const sinceRaw = c.req.query("since");
  const since = sinceRaw ? Number(sinceRaw) : 0;
  if (!Number.isFinite(since) || since < 0) return c.text("bad since", 400);

  const config = getConfig();
  const core = createCore(resolveActiveKgPath(config));
  try {
    const result = exportSharedKg(core.client.raw, { since, workspaceId: config.workspace });
    return new Response(result.ndjson, {
      status: 200,
      headers: {
        "content-type": "application/x-ndjson; charset=utf-8",
        "x-kg-watermark": String(result.watermark),
        "x-kg-count": String(result.count),
      },
    });
  } finally {
    core.close();
  }
});
