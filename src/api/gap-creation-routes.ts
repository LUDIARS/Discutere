import Database from "better-sqlite3";
import { Hono } from "hono";
import type { Context } from "hono";

import { getUserId, getUserRole } from "../middleware/auth.js";
import { createCore } from "../core/index.js";
import { resolveActiveKgPath } from "../core/kg-registry.js";
import { getConfig } from "../config.js";
import { PersonasRepo } from "../persona-engine/db/personas-repo.js";
import { applyPersonaEngineMigrations } from "../persona-engine/db/migrations.js";
import { seedHeadlessDiscussion } from "../discussion-seed/seed.js";

export const gapCreationRoutes = new Hono();

gapCreationRoutes.post("/admin/gaps", (context) => createGap(context));

interface GapCreationInput {
  title: string;
  description: string;
  gameId?: string;
}

/** @implements SPEC-THALEIA-GAP-CREATION */
async function createGap(c: Context): Promise<Response> {
  const guard = requireAdmin(c);
  if (guard) return guard;
  const mediaType = c.req.header("content-type")?.split(";", 1)[0].trim().toLowerCase();
  if (mediaType !== "application/json") {
    return c.json({ error: "content-type must be application/json" }, 415);
  }

  const body = await c.req.json<unknown>().catch(() => null);
  const input = parseGapCreationInput(body);
  if (!input) {
    return c.json(
      { error: "title and description are required; gameId must be non-empty when supplied" },
      400
    );
  }

  const config = getConfig();
  const core = createCore(resolveActiveKgPath(config));
  let personaDb: Database.Database | null = null;
  try {
    if (input.gameId && !gameExists(core, config.workspace, input.gameId)) {
      return c.json({ error: "gameId must identify a game in the current workspace" }, 400);
    }

    personaDb = new Database(config.personaEngine.dbPath);
    applyPersonaEngineMigrations(personaDb);
    const seeded = seedHeadlessDiscussion({
      core,
      personas: new PersonasRepo(personaDb),
      workspaceId: config.workspace,
      topic: { title: input.title, description: input.description },
      ...(input.gameId ? { gameId: input.gameId } : {}),
      origin: "api:admin-gap",
    });
    return c.json(seeded, 201);
  } catch {
    // Database errors can contain local paths and SQL details; keep them out of the response.
    return c.json({ error: "gap creation unavailable" }, 503);
  } finally {
    try {
      personaDb?.close();
    } finally {
      core.close();
    }
  }
}

/** @implements SPEC-THALEIA-GAP-CREATION */
function parseGapCreationInput(body: unknown): GapCreationInput | null {
  if (!body || typeof body !== "object" || Array.isArray(body)) return null;
  const input = body as Record<string, unknown>;
  if (typeof input.title !== "string" || input.title.trim() === "") return null;
  if (typeof input.description !== "string" || input.description.trim() === "") return null;
  if (input.gameId !== undefined && (typeof input.gameId !== "string" || input.gameId.trim() === "")) {
    return null;
  }
  return {
    title: input.title.trim(),
    description: input.description.trim(),
    ...(typeof input.gameId === "string" ? { gameId: input.gameId.trim() } : {}),
  };
}

/** @implements SPEC-THALEIA-GAP-CREATION */
function gameExists(
  core: ReturnType<typeof createCore>,
  workspaceId: string,
  gameId: string
): boolean {
  const row = core.client.raw
    .prepare("SELECT 1 FROM games WHERE workspace_id = ? AND id = ?")
    .get(workspaceId, gameId);
  return row !== undefined;
}

/** @implements SPEC-THALEIA-GAP-CREATION */
function requireAdmin(c: Context): Response | null {
  const userId = getUserId(c);
  const role = getUserRole(c);
  if (!userId) return c.json({ error: "Authentication required" }, 401);
  if (role !== "admin") return c.json({ error: "admin role required" }, 403);
  return null;
}
