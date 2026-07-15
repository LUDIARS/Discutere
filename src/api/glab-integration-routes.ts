import { Hono } from "hono";

import { glabLaunchStore } from "../integrations/glab-launch.js";
import { isLoopbackRequest } from "../middleware/auth.js";

export const glabIntegrationRoutes = new Hono();

glabIntegrationRoutes.post("/api/integrations/glab/launch", (c) => {
  if (!isLoopbackRequest(c)) return c.json({ ok: false, error: "loopback_required" }, 403);
  const cernereUserId = c.req.header("X-Cernere-User-Id")?.trim() ?? "";
  if (!cernereUserId || cernereUserId.length > 200) {
    return c.json({ ok: false, error: "invalid_cernere_user_id" }, 400);
  }
  const ticket = glabLaunchStore.createLaunch(cernereUserId);
  return c.json({
    ok: true,
    path: `/flow?glab_launch=${encodeURIComponent(ticket)}`,
    expiresInSeconds: 60,
  });
});
