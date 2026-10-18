import { timingSafeEqual } from "node:crypto";
import { Hono } from "hono";
import type Database from "better-sqlite3";
import { getFlowDb } from "../flow/db/connection.js";
import { isLoopbackRequest } from "../middleware/auth.js";
import { ensureConclusionCacheFresh } from "../visualize/conclusion-cache.js";
import { publicationState, publishedConclusions, setPublication } from "../integrations/conclusion-publication.js";
import { PUBLICATION_EDITOR } from "./conclusion-publication-editor.js";
import { getConfig } from "../config.js";

/** Separate server credential: never accept a client-supplied audience or role header. */
export function createConclusionPublicationRoutes(
  database: () => Database.Database = getFlowDb,
  token: () => string = () => getConfig().glabConclusions?.token ?? "",
): Hono {
  const routes = new Hono();
  routes.use("*", async (c, next) => {
    c.header("Cache-Control", "private, no-store");
    await next();
  });
  routes.use("/integrations/glab/conclusions*", async (c, next) => {
    const expected = Buffer.from(token());
    if (expected.length < 32) return c.json({ error: "conclusions_unconfigured" }, 503);
    const actual = Buffer.from(/^Bearer (\S+)$/.exec(c.req.header("authorization") ?? "")?.[1] ?? "");
    if (actual.length !== expected.length || !timingSafeEqual(actual, expected)) return c.json({ error: "unauthorized" }, 401);
    await next();
  });
  routes.get("/integrations/glab/conclusions", (c) => {
    const db = database();
    ensureConclusionCacheFresh(db);
    const id = c.req.query("id");
    const results = publishedConclusions(db, id);
    if (id !== undefined && results.length === 0) return c.json({ error: "not_found" }, 404);
    return c.json({ conclusions: results });
  });
  // Publication changes are local operator actions, not GLAB actions. Host + Origin
  // checks also reject tunnel/proxy requests and cross-site browser submissions.
  routes.use("/admin/conclusion-publication*", async (c, next) => {
    const url = new URL(c.req.url);
    if (!isLoopbackRequest(c) || !["localhost", "127.0.0.1", "[::1]"].includes(url.hostname)) return c.json({ error: "local_operator_required" }, 403);
    const origin = c.req.header("origin");
    if (origin && origin !== url.origin) return c.json({ error: "origin_denied" }, 403);
    await next();
  });
  routes.get("/admin/conclusion-publication/editor", (c) => c.html(PUBLICATION_EDITOR));
  routes.get("/admin/conclusion-publication", (c) => {
    const db = database();
    ensureConclusionCacheFresh(db);
    const state = publicationState(db, c.req.query("id") ?? "");
    return state ? c.json(state) : c.json({ error: "not_found" }, 404);
  });
  routes.put("/admin/conclusion-publication", async (c) => {
    if (!c.req.header("content-type")?.startsWith("application/json")) return c.json({ error: "json_required" }, 415);
    const body: unknown = await c.req.json().catch(() => null);
    if (!body || typeof body !== "object" || Array.isArray(body)) return c.json({ error: "invalid_input" }, 400);
    const input = body as Record<string, unknown>;
    if (Object.keys(input).some(key => !["id", "publishable", "scope", "revision"].includes(key)) || typeof input.id !== "string" || !input.id || input.id.length > 500 || typeof input.publishable !== "boolean" || typeof input.revision !== "string" || typeof input.scope !== "string" || !["private", "public", "glab"].includes(input.scope)) return c.json({ error: "invalid_input" }, 400);
    const db = database();
    ensureConclusionCacheFresh(db);
    if (!setPublication(db, input.id, input.publishable, input.scope as "private" | "public" | "glab", input.revision, Date.now())) return c.json({ error: "conclusion_changed_or_unavailable" }, 409);
    return c.json(publicationState(db, input.id));
  });
  return routes;
}

export const conclusionPublicationRoutes = createConclusionPublicationRoutes();
