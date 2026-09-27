// @implements DI-EXTERNAL-DISCUSSION
import { timingSafeEqual } from "node:crypto";
import { Hono } from "hono";
import { bodyLimit } from "hono/body-limit";
import { parseConversation } from "../external-discussion/contracts.js";
import type { ExternalDiscussionService } from "../external-discussion/service.js";

export function externalDiscussionRoutes(service: ExternalDiscussionService | null, secret: string): Hono {
  const app = new Hono();
  let running = 0;
  app.use("*", bodyLimit({ maxSize: 160_000 }));
  app.post("/consider", async c => {
    if (!secret || !service) return c.json({ error: "external discussion unavailable" }, 503);
    const actual = Buffer.from(c.req.header("authorization") ?? "");
    const expected = Buffer.from(`Bearer ${secret}`);
    if (actual.length !== expected.length || !timingSafeEqual(actual, expected)) return c.json({ error: "unauthorized" }, 401);
    let input;
    try { input = parseConversation(await c.req.json()); }
    catch { return c.json({ error: "invalid conversation" }, 400); }
    if (running >= 4) return c.json({ error: "discussion busy" }, 429);
    running++;
    try { return c.json(await service.consider(input)); }
    catch { return c.json({ error: "discussion generation failed" }, 503); }
    finally { running--; }
  });
  return app;
}
