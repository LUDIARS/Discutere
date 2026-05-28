import type Database from "better-sqlite3";
import { EVENT_TYPES, type DomainEvent } from "./event-types.js";

function nowMs(event: DomainEvent): number { return event.payload.at ?? event.createdAt; }

export function applyEventProjection(db: Database.Database, event: DomainEvent): void {
  const t = nowMs(event);
  switch (event.eventType) {
    case EVENT_TYPES.PersonCreated:
    case EVENT_TYPES.PersonUpdated:
      db.prepare(`INSERT INTO people (id, workspace_id, name, role, created_at, updated_at)
        VALUES (?, ?, ?, ?, ?, ?)
        ON CONFLICT(id) DO UPDATE SET name = excluded.name, role = excluded.role, updated_at = excluded.updated_at`)
        .run(event.payload.id, event.payload.workspaceId, (event.payload as any).name, (event.payload as any).role ?? null, t, t);
      return;

    case EVENT_TYPES.SessionCreated:
    case EVENT_TYPES.SessionUpdated:
    case EVENT_TYPES.SessionEnded:
      db.prepare(`INSERT INTO sessions (id, workspace_id, title, started_at, ended_at, mode, scene, created_at, updated_at)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(id) DO UPDATE SET title=excluded.title, started_at=excluded.started_at, ended_at=excluded.ended_at, mode=excluded.mode, scene=excluded.scene, updated_at=excluded.updated_at`)
        .run(event.payload.id, event.payload.workspaceId, (event.payload as any).title ?? "session", (event.payload as any).startedAt ?? t, (event.payload as any).endedAt ?? null, (event.payload as any).mode ?? null, (event.payload as any).scene ?? null, t, t);
      return;

    case EVENT_TYPES.UtteranceCreated:
      db.prepare(`INSERT INTO utterances (id, workspace_id, session_id, speaker_id, raw_content, normalized_content, posted_at, responds_to, created_at, updated_at)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`)
        .run(event.payload.id, event.payload.workspaceId, (event.payload as any).sessionId, (event.payload as any).speakerId ?? null, (event.payload as any).rawContent, (event.payload as any).normalizedContent ?? null, (event.payload as any).postedAt, (event.payload as any).respondsTo ?? null, t, t);
      return;

    case EVENT_TYPES.UtteranceNormalized:
      db.prepare("UPDATE utterances SET normalized_content = ?, updated_at = ? WHERE id = ?").run((event.payload as any).normalizedContent ?? null, t, event.payload.id);
      return;

    case EVENT_TYPES.ReactionAdded:
      db.prepare(`INSERT INTO reactions (id, workspace_id, utterance_id, actor_id, reaction_type, intensity, created_at, updated_at)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(id) DO UPDATE SET reaction_type=excluded.reaction_type, intensity=excluded.intensity, updated_at=excluded.updated_at`)
        .run(event.payload.id, event.payload.workspaceId, (event.payload as any).utteranceId, (event.payload as any).actorId ?? null, (event.payload as any).reactionType, (event.payload as any).intensity ?? null, t, t);
      return;

    case EVENT_TYPES.MechanicProposed:
    case EVENT_TYPES.MechanicRefined:
      db.prepare(`INSERT INTO mechanics (id, workspace_id, game_id, name, description, intends, created_at, updated_at)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(id) DO UPDATE SET game_id=excluded.game_id, name=excluded.name, description=excluded.description, intends=excluded.intends, updated_at=excluded.updated_at`)
        .run(event.payload.id, event.payload.workspaceId, (event.payload as any).gameId ?? null, (event.payload as any).name, (event.payload as any).description ?? null, (event.payload as any).intends ?? null, t, t);
      return;

    case EVENT_TYPES.AffectMeasured:
      db.prepare(`INSERT INTO affects (id, workspace_id, session_id, subject_id, mood, score, created_at, updated_at)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(id) DO UPDATE SET mood=excluded.mood, score=excluded.score, updated_at=excluded.updated_at`)
        .run(event.payload.id, event.payload.workspaceId, (event.payload as any).sessionId ?? null, (event.payload as any).subjectId ?? null, (event.payload as any).mood, (event.payload as any).score ?? null, t, t);
      return;

    case EVENT_TYPES.PlayContextCaptured:
      db.prepare(`INSERT INTO play_contexts (id, workspace_id, game_id, platform, mode, created_at, updated_at)
        VALUES (?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(id) DO UPDATE SET game_id=excluded.game_id, platform=excluded.platform, mode=excluded.mode, updated_at=excluded.updated_at`)
        .run(event.payload.id, event.payload.workspaceId, (event.payload as any).gameId ?? null, (event.payload as any).platform ?? null, (event.payload as any).mode ?? null, t, t);
      return;

    case EVENT_TYPES.DesignGapDetected:
    case EVENT_TYPES.DesignGapUpdated:
      db.prepare(`INSERT INTO design_gaps (id, workspace_id, game_id, title, description, status, created_at, updated_at)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(id) DO UPDATE SET game_id=excluded.game_id, title=excluded.title, description=excluded.description, status=excluded.status, updated_at=excluded.updated_at`)
        .run(event.payload.id, event.payload.workspaceId, (event.payload as any).gameId ?? null, (event.payload as any).title, (event.payload as any).description ?? null, (event.payload as any).status ?? null, t, t);
      return;

    case EVENT_TYPES.HypothesisCreated:
    case EVENT_TYPES.HypothesisUpdated:
    case EVENT_TYPES.HypothesisProposed:
    case EVENT_TYPES.HypothesisIntegrated:
      db.prepare(`INSERT INTO hypotheses (id, workspace_id, design_gap_id, statement, status, integrated, validated_by_emotion, refs_json, created_at, updated_at)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(id) DO UPDATE SET design_gap_id=excluded.design_gap_id, statement=excluded.statement, status=excluded.status, integrated=excluded.integrated, validated_by_emotion=excluded.validated_by_emotion, refs_json=excluded.refs_json, updated_at=excluded.updated_at`)
        .run(event.payload.id, event.payload.workspaceId, (event.payload as any).designGapId ?? null, (event.payload as any).statement, (event.payload as any).status ?? null, (event.payload as any).integrated ? 1 : 0, (event.payload as any).validatedByEmotion ? 1 : 0, JSON.stringify((event.payload as any).refs ?? []), t, t);
      return;

    default:
      return;
  }
}
