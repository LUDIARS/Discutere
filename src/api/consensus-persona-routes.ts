import Database from "better-sqlite3";
import { Hono } from "hono";
import type { Context } from "hono";

import { getUserId, getUserRole } from "../middleware/auth.js";
import { createCore } from "../core/index.js";
import { resolveActiveKgPath } from "../core/kg-registry.js";
import { getConfig } from "../config.js";
import { FACILITATOR_PERSONA_ID } from "../persona-engine/facilitator/facilitator.js";
import {
  aggregatePersonaConsensus,
  type PersonaConsensusSource,
  type PersonaConsensusSummary,
} from "./consensus-persona-aggregation.js";

export const consensusPersonaRoutes = new Hono();

consensusPersonaRoutes.get("/admin/consensus/:gapId/personas", (context) =>
  getPersonaConsensus(context)
);

/** @implements SPEC-THALEIA-PERSONA-CONSENSUS */
function getPersonaConsensus(c: Context): Response {
  const guard = requireAdmin(c);
  if (guard) return guard;
  c.header("Cache-Control", "private, no-store");

  const config = getConfig();
  const gapId = c.req.param("gapId");
  const core = createCore(resolveActiveKgPath(config));
  let personaDb: Database.Database | null = null;
  try {
    const session = core.client.raw
      .prepare("SELECT id FROM sessions WHERE workspace_id = ? AND title = ? ORDER BY started_at DESC LIMIT 1")
      .get(config.workspace, `discussion-of-gap:${gapId}`) as { id: string } | undefined;
    if (!session) return c.json({ gapId, personas: [] });

    personaDb = new Database(config.personaEngine.dbPath, { readonly: true, fileMustExist: true });
    personaDb.pragma("busy_timeout = 2000");
    return c.json({ gapId, personas: loadPersonaConsensus(core, personaDb, session.id) });
  } catch {
    // Database errors can contain local paths and SQL details; keep them out of the response.
    return c.json({ error: "persona consensus unavailable" }, 503);
  } finally {
    try {
      personaDb?.close();
    } finally {
      core.close();
    }
  }
}

/** @implements SPEC-THALEIA-PERSONA-CONSENSUS */
function loadPersonaConsensus(
  core: ReturnType<typeof createCore>,
  personaDb: Database.Database,
  sessionId: string
): PersonaConsensusSummary[] {
  const utterances = core.client.raw
    .prepare(
      `SELECT u.id, u.speaker_id AS speakerId, u.raw_content AS content, u.posted_at AS postedAt,
              cs.agree, cs.score, cs.reasoning, COALESCE(os.score, 0) AS opinionScore
         FROM utterances u
         LEFT JOIN consensus_scores cs ON cs.utterance_id = u.id
         LEFT JOIN opinion_scores os ON os.target_id = u.id
        WHERE u.session_id = ?
          AND u.speaker_id LIKE 'persona:%'
          AND u.speaker_id NOT LIKE 'ext:%'
          AND u.speaker_id <> ?
        ORDER BY u.posted_at ASC`
    )
    .all(sessionId, `persona:${FACILITATOR_PERSONA_ID}`) as Array<{
    id: string;
    speakerId: string;
    content: string;
    postedAt: number;
    agree: number | null;
    score: number | null;
    reasoning: string | null;
    opinionScore: number;
  }>;
  const personaById = new Map(
    (personaDb.prepare("SELECT id, display_name, traits FROM personas").all() as Array<{
      id: string;
      display_name: string;
      traits: string;
    }>).map((persona) => [persona.id, persona])
  );
  const rows: PersonaConsensusSource[] = utterances.flatMap((utterance) => {
    const personaId = utterance.speakerId.slice("persona:".length);
    const persona = personaById.get(personaId);
    if (!persona) return [];
    return [{
      id: utterance.id,
      content: utterance.content,
      postedAt: utterance.postedAt,
      agree: utterance.agree === null ? null : utterance.agree === 1,
      score: utterance.score,
      reasoning: utterance.reasoning,
      opinionScore: utterance.opinionScore,
      personaId,
      displayName: persona.display_name,
      traits: parseTraits(persona.traits),
    }];
  });
  return aggregatePersonaConsensus(rows);
}

/** @implements SPEC-THALEIA-PERSONA-CONSENSUS */
function parseTraits(raw: string): string[] {
  try {
    const parsed: unknown = JSON.parse(raw);
    return Array.isArray(parsed) && parsed.every((trait) => typeof trait === "string") ? parsed : [];
  } catch {
    return [];
  }
}

/** @implements SPEC-THALEIA-PERSONA-CONSENSUS */
function requireAdmin(c: Context): Response | null {
  const userId = getUserId(c);
  const role = getUserRole(c);
  if (!userId) return c.json({ error: "Authentication required" }, 401);
  if (role !== "admin") return c.json({ error: "admin role required" }, 403);
  return null;
}
