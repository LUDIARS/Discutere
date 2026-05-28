import { randomUUID } from "node:crypto";

import type { KuzuClient } from "../db/kuzu-client.js";
import { EVENT_TYPES, type DomainEvent, type EventPayload } from "../events/event-types.js";
import { EventLog } from "../events/event-log.js";

export interface CoreContext {
  client: KuzuClient;
  eventLog: EventLog;
}

export function makeContext(client: KuzuClient, eventLog: EventLog): CoreContext {
  return { client, eventLog };
}

function append(ctx: CoreContext, eventType: DomainEvent["eventType"], payload: EventPayload): string {
  return ctx.eventLog.appendEvent({ eventType, payload }).id;
}

function getById<T>(ctx: CoreContext, table: string, id: string): T | undefined {
  return ctx.client.raw.prepare(`SELECT * FROM ${table} WHERE id = ?`).get(id) as T | undefined;
}

function listByWorkspace<T>(ctx: CoreContext, table: string, workspaceId: string): T[] {
  return ctx.client.raw
    .prepare(`SELECT * FROM ${table} WHERE workspace_id = ? ORDER BY created_at DESC`)
    .all(workspaceId) as T[];
}

export function createPersonRepo(ctx: CoreContext) {
  return {
    create(input: { id?: string; workspaceId: string; name: string; role?: string }): string {
      const id = input.id ?? randomUUID();
      append(ctx, EVENT_TYPES.PersonCreated, { ...input, id });
      return id;
    },
    get(id: string) { return getById(ctx, "people", id); },
    list(workspaceId: string) { return listByWorkspace(ctx, "people", workspaceId); },
    update(id: string, patch: { workspaceId: string; name: string; role?: string }) {
      append(ctx, EVENT_TYPES.PersonUpdated, { ...patch, id });
    },
  };
}

export function createGameRepo(ctx: CoreContext) {
  return {
    create(input: { id?: string; workspaceId: string; title: string; genre?: string }): string {
      const id = input.id ?? randomUUID();
      const at = Date.now();
      ctx.client.raw.prepare(`INSERT INTO games (id, workspace_id, title, genre, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?)`).run(id, input.workspaceId, input.title, input.genre ?? null, at, at);
      ctx.eventLog.appendEvent({ eventType: EVENT_TYPES.PlayContextCaptured, payload: { id: randomUUID(), workspaceId: input.workspaceId, gameId: id } });
      return id;
    },
    get(id: string) { return getById(ctx, "games", id); },
    list(workspaceId: string) { return listByWorkspace(ctx, "games", workspaceId); },
    update(id: string, patch: { workspaceId: string; title: string; genre?: string }) {
      ctx.client.raw.prepare("UPDATE games SET title = ?, genre = ?, updated_at = ? WHERE id = ?").run(patch.title, patch.genre ?? null, Date.now(), id);
    },
  };
}

export function createMechanicRepo(ctx: CoreContext) {
  return {
    create(input: { id?: string; workspaceId: string; gameId?: string; name: string; description?: string }): string {
      const id = input.id ?? randomUUID();
      append(ctx, EVENT_TYPES.MechanicProposed, { ...input, id });
      return id;
    },
    get(id: string) { return getById(ctx, "mechanics", id); },
    list(workspaceId: string) { return listByWorkspace(ctx, "mechanics", workspaceId); },
    update(id: string, patch: { workspaceId: string; gameId?: string; name: string; description?: string }) {
      append(ctx, EVENT_TYPES.MechanicProposed, { ...patch, id });
    },
  };
}

export function createAestheticRepo(ctx: CoreContext) {
  return {
    create(input: { id?: string; workspaceId: string; gameId?: string; name: string; description?: string }): string {
      const id = input.id ?? randomUUID();
      const at = Date.now();
      ctx.client.raw.prepare(`INSERT INTO aesthetics (id, workspace_id, game_id, name, description, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?)`).run(id, input.workspaceId, input.gameId ?? null, input.name, input.description ?? null, at, at);
      return id;
    },
    get(id: string) { return getById(ctx, "aesthetics", id); },
    list(workspaceId: string) { return listByWorkspace(ctx, "aesthetics", workspaceId); },
    update(id: string, patch: { name: string; description?: string }) {
      ctx.client.raw.prepare("UPDATE aesthetics SET name = ?, description = ?, updated_at = ? WHERE id = ?").run(patch.name, patch.description ?? null, Date.now(), id);
    },
  };
}

export function createAffectRepo(ctx: CoreContext) {
  return {
    create(input: { id?: string; workspaceId: string; sessionId?: string; subjectId?: string; mood: string; score?: number }): string {
      const id = input.id ?? randomUUID();
      append(ctx, EVENT_TYPES.AffectMeasured, { ...input, id });
      return id;
    },
    get(id: string) { return getById(ctx, "affects", id); },
    list(workspaceId: string) { return listByWorkspace(ctx, "affects", workspaceId); },
    update(id: string, patch: { workspaceId: string; sessionId?: string; subjectId?: string; mood: string; score?: number }) {
      append(ctx, EVENT_TYPES.AffectMeasured, { ...patch, id });
    },
  };
}

export function createPlayContextRepo(ctx: CoreContext) {
  return {
    create(input: { id?: string; workspaceId: string; gameId?: string; platform?: string; mode?: string }): string {
      const id = input.id ?? randomUUID();
      append(ctx, EVENT_TYPES.PlayContextCaptured, { ...input, id });
      return id;
    },
    get(id: string) { return getById(ctx, "play_contexts", id); },
    list(workspaceId: string) { return listByWorkspace(ctx, "play_contexts", workspaceId); },
    update(id: string, patch: { workspaceId: string; gameId?: string; platform?: string; mode?: string }) {
      append(ctx, EVENT_TYPES.PlayContextCaptured, { ...patch, id });
    },
  };
}

export function createSessionRepo(ctx: CoreContext) {
  return {
    create(input: { id?: string; workspaceId: string; title: string; startedAt: number; endedAt?: number; mode?: string; scene?: string }): string {
      const id = input.id ?? randomUUID();
      append(ctx, EVENT_TYPES.SessionCreated, { ...input, id });
      return id;
    },
    get(id: string) { return getById(ctx, "sessions", id); },
    list(workspaceId: string) { return listByWorkspace(ctx, "sessions", workspaceId); },
    update(id: string, patch: { workspaceId: string; title: string; startedAt: number; endedAt?: number; mode?: string; scene?: string }) {
      append(ctx, EVENT_TYPES.SessionUpdated, { ...patch, id });
    },
  };
}

export function createUtteranceRepo(ctx: CoreContext) {
  return {
    create(input: { id?: string; workspaceId: string; sessionId: string; speakerId?: string; rawContent: string; normalizedContent?: string; postedAt: number; respondsTo?: string }): string {
      const id = input.id ?? randomUUID();
      append(ctx, EVENT_TYPES.UtteranceCreated, { ...input, id });
      return id;
    },
    get(id: string) { return getById<{ raw_content: string; [k: string]: unknown }>(ctx, "utterances", id); },
    list(workspaceId: string) { return listByWorkspace(ctx, "utterances", workspaceId); },
    update(id: string, patch: { workspaceId: string; normalizedContent: string; rawContent?: string }) {
      const current = getById<{ raw_content: string }>(ctx, "utterances", id);
      if (!current) throw new Error("utterance not found");
      if (patch.rawContent !== undefined && patch.rawContent !== current.raw_content) {
        throw new Error("Utterance.raw_content is immutable");
      }
      append(ctx, EVENT_TYPES.UtteranceNormalized, { id, workspaceId: patch.workspaceId, normalizedContent: patch.normalizedContent });
    },
  };
}

export function createReactionRepo(ctx: CoreContext) {
  return {
    create(input: { id?: string; workspaceId: string; utteranceId: string; actorId?: string; reactionType: string; intensity?: number }): string {
      const id = input.id ?? randomUUID();
      append(ctx, EVENT_TYPES.ReactionAdded, { ...input, id });
      return id;
    },
    get(id: string) { return getById(ctx, "reactions", id); },
    list(workspaceId: string) { return listByWorkspace(ctx, "reactions", workspaceId); },
    update(id: string, patch: { workspaceId: string; utteranceId: string; actorId?: string; reactionType: string; intensity?: number }) {
      append(ctx, EVENT_TYPES.ReactionAdded, { ...patch, id });
    },
  };
}

export function createDesignGapRepo(ctx: CoreContext) {
  return {
    create(input: { id?: string; workspaceId: string; gameId?: string; title: string; description?: string; status?: string }): string {
      const id = input.id ?? randomUUID();
      append(ctx, EVENT_TYPES.DesignGapDetected, { ...input, id });
      return id;
    },
    get(id: string) { return getById(ctx, "design_gaps", id); },
    list(workspaceId: string) { return listByWorkspace(ctx, "design_gaps", workspaceId); },
    update(id: string, patch: { workspaceId: string; gameId?: string; title: string; description?: string; status?: string }) {
      append(ctx, EVENT_TYPES.DesignGapUpdated, { ...patch, id });
    },
  };
}

export function createHypothesisRepo(ctx: CoreContext) {
  return {
    create(input: { id?: string; workspaceId: string; designGapId?: string; statement: string; status?: string; integrated?: boolean }): string {
      const id = input.id ?? randomUUID();
      append(ctx, EVENT_TYPES.HypothesisCreated, { ...input, id });
      return id;
    },
    get(id: string) { return getById(ctx, "hypotheses", id); },
    list(workspaceId: string) { return listByWorkspace(ctx, "hypotheses", workspaceId); },
    update(id: string, patch: { workspaceId: string; designGapId?: string; statement: string; status?: string; integrated?: boolean }) {
      append(ctx, EVENT_TYPES.HypothesisUpdated, { ...patch, id });
    },
  };
}
