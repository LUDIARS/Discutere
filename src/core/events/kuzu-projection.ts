/**
 * Kuzu グラフへのイベント投影 — spec/core/KUZU-MIGRATION.md §4。
 *
 * `applyEventProjection`（SQLite 側）と対になる Kuzu 版。
 * 全 MERGE は冪等（同一 id を再実行しても安全）。
 * 関係 MERGE は双方のノードが既存であることを前提とする。
 * ノードが未存在なら `tryRel` がエラーをスキップ（結果整合 OK、replay で補完）。
 */

import type { KuzuGraphClient } from "../db/kuzu-graph-client.js";
import type { KuzuValue } from "kuzu";
import { EVENT_TYPES, type DomainEvent } from "./event-types.js";

type Params = Record<string, KuzuValue>;

// eslint-disable-next-line @typescript-eslint/no-explicit-any
function payload(event: DomainEvent): Record<string, any> {
  return event.payload as unknown as Record<string, unknown>;
}

function s(v: unknown): string { return v != null ? String(v) : ""; }
function n(v: unknown): number { return v != null ? Number(v) : 0; }
function d(v: unknown): number { return v != null ? Number(v) : 0.0; }
function b(v: unknown): boolean { return Boolean(v); }

export async function applyKuzuProjection(graph: KuzuGraphClient, event: DomainEvent): Promise<void> {
  const p = payload(event);
  const t = n(p.at ?? event.createdAt);
  const id = s(p.id);
  const ws = s(p.workspaceId);

  switch (event.eventType) {
    case EVENT_TYPES.PersonCreated:
    case EVENT_TYPES.PersonUpdated:
      await graph.run(
        `MERGE (n:Person {id: $id})
         SET n.workspace_id = $ws, n.name = $name, n.role = $role,
             n.created_at = CASE WHEN n.created_at = 0 THEN $t ELSE n.created_at END,
             n.updated_at = $t`,
        { id, ws, name: s(p.name), role: s(p.role), t } satisfies Params,
      );
      return;

    case EVENT_TYPES.SessionCreated:
    case EVENT_TYPES.SessionUpdated:
    case EVENT_TYPES.SessionEnded:
      await graph.run(
        `MERGE (n:Session {id: $id})
         SET n.workspace_id = $ws, n.title = $title,
             n.started_at = $startedAt, n.ended_at = $endedAt,
             n.mode = $mode, n.scene = $scene,
             n.created_at = CASE WHEN n.created_at = 0 THEN $t ELSE n.created_at END,
             n.updated_at = $t`,
        {
          id, ws, title: s(p.title ?? "session"),
          startedAt: n(p.startedAt ?? t), endedAt: n(p.endedAt ?? 0),
          mode: s(p.mode), scene: s(p.scene), t,
        } satisfies Params,
      );
      return;

    case EVENT_TYPES.UtteranceCreated: {
      await graph.run(
        `MERGE (n:Utterance {id: $id})
         SET n.workspace_id = $ws, n.raw_content = $raw,
             n.normalized_content = $norm, n.posted_at = $postedAt,
             n.created_at = CASE WHEN n.created_at = 0 THEN $t ELSE n.created_at END,
             n.updated_at = $t`,
        {
          id, ws, raw: s(p.rawContent), norm: s(p.normalizedContent),
          postedAt: n(p.postedAt ?? t), t,
        } satisfies Params,
      );
      if (p.speakerId) {
        await tryRel(graph,
          `MATCH (u:Utterance {id: $uid}), (pe:Person {id: $pid})
           MERGE (u)-[:SPOKEN_BY]->(pe)`,
          { uid: id, pid: s(p.speakerId) },
        );
      }
      if (p.sessionId) {
        await tryRel(graph,
          `MATCH (u:Utterance {id: $uid}), (s:Session {id: $sid})
           MERGE (u)-[:IN_SESSION]->(s)`,
          { uid: id, sid: s(p.sessionId) },
        );
      }
      if (p.respondsTo) {
        await tryRel(graph,
          `MATCH (u:Utterance {id: $uid}), (r:Utterance {id: $rid})
           MERGE (u)-[:RESPONDS_TO]->(r)`,
          { uid: id, rid: s(p.respondsTo) },
        );
      }
      return;
    }

    case EVENT_TYPES.UtteranceNormalized:
      await graph.run(
        `MATCH (n:Utterance {id: $id}) SET n.normalized_content = $norm, n.updated_at = $t`,
        { id, norm: s(p.normalizedContent), t } satisfies Params,
      );
      return;

    case EVENT_TYPES.ReactionAdded:
      await graph.run(
        `MERGE (n:Reaction {id: $id})
         SET n.workspace_id = $ws, n.reaction_type = $rtype,
             n.intensity = $intensity,
             n.created_at = CASE WHEN n.created_at = 0 THEN $t ELSE n.created_at END,
             n.updated_at = $t`,
        { id, ws, rtype: s(p.reactionType), intensity: d(p.intensity), t } satisfies Params,
      );
      if (p.utteranceId) {
        await tryRel(graph,
          `MATCH (r:Reaction {id: $rid}), (u:Utterance {id: $uid})
           MERGE (r)-[:REACTS_ON]->(u)`,
          { rid: id, uid: s(p.utteranceId) },
        );
      }
      if (p.actorId) {
        await tryRel(graph,
          `MATCH (r:Reaction {id: $rid}), (pe:Person {id: $pid})
           MERGE (r)-[:REACTED_BY]->(pe)`,
          { rid: id, pid: s(p.actorId) },
        );
      }
      return;

    case EVENT_TYPES.MechanicProposed:
    case EVENT_TYPES.MechanicRefined:
      await graph.run(
        `MERGE (n:Mechanic {id: $id})
         SET n.workspace_id = $ws, n.name = $name,
             n.description = $desc, n.intends = $intends,
             n.created_at = CASE WHEN n.created_at = 0 THEN $t ELSE n.created_at END,
             n.updated_at = $t`,
        { id, ws, name: s(p.name), desc: s(p.description), intends: s(p.intends), t } satisfies Params,
      );
      if (p.gameId) {
        await tryRel(graph,
          `MATCH (m:Mechanic {id: $mid}), (g:Game {id: $gid})
           MERGE (m)-[:OF_GAME]->(g)`,
          { mid: id, gid: s(p.gameId) },
        );
      }
      return;

    case EVENT_TYPES.AffectMeasured:
      await graph.run(
        `MERGE (n:Affect {id: $id})
         SET n.workspace_id = $ws, n.mood = $mood, n.score = $score,
             n.created_at = CASE WHEN n.created_at = 0 THEN $t ELSE n.created_at END,
             n.updated_at = $t`,
        { id, ws, mood: s(p.mood), score: d(p.score), t } satisfies Params,
      );
      if (p.sessionId) {
        await tryRel(graph,
          `MATCH (a:Affect {id: $aid}), (s:Session {id: $sid})
           MERGE (a)-[:IN_SESSION]->(s)`,
          { aid: id, sid: s(p.sessionId) },
        );
      }
      return;

    case EVENT_TYPES.DesignGapDetected:
    case EVENT_TYPES.DesignGapUpdated:
      await graph.run(
        `MERGE (n:DesignGap {id: $id})
         SET n.workspace_id = $ws, n.title = $title,
             n.description = $desc, n.status = $status,
             n.created_at = CASE WHEN n.created_at = 0 THEN $t ELSE n.created_at END,
             n.updated_at = $t`,
        { id, ws, title: s(p.title), desc: s(p.description), status: s(p.status), t } satisfies Params,
      );
      if (p.gameId) {
        await tryRel(graph,
          `MATCH (dg:DesignGap {id: $dgid}), (g:Game {id: $gid})
           MERGE (dg)-[:IN_GAME]->(g)`,
          { dgid: id, gid: s(p.gameId) },
        );
      }
      return;

    case EVENT_TYPES.HypothesisCreated:
    case EVENT_TYPES.HypothesisUpdated:
    case EVENT_TYPES.HypothesisProposed:
    case EVENT_TYPES.HypothesisDebated:
    case EVENT_TYPES.HypothesisValidated:
    case EVENT_TYPES.HypothesisIntegrated:
    case EVENT_TYPES.HypothesisRejected:
    case EVENT_TYPES.HypothesisStaleFlagged:
      await graph.run(
        `MERGE (n:Hypothesis {id: $id})
         SET n.workspace_id = $ws, n.statement = $stmt,
             n.status = $status, n.integrated = $integrated,
             n.validated_by_emotion = $validated,
             n.stale_flagged = $stale, n.refs_json = $refs,
             n.created_at = CASE WHEN n.created_at = 0 THEN $t ELSE n.created_at END,
             n.updated_at = $t`,
        {
          id, ws, stmt: s(p.statement), status: s(p.status),
          integrated: b(p.integrated), validated: b(p.validatedByEmotion),
          stale: b(p.staleFlagged), refs: JSON.stringify(p.refs ?? []), t,
        } satisfies Params,
      );
      if (p.designGapId) {
        await tryRel(graph,
          `MATCH (h:Hypothesis {id: $hid}), (dg:DesignGap {id: $dgid})
           MERGE (h)-[:ADDRESSES]->(dg)`,
          { hid: id, dgid: s(p.designGapId) },
        );
      }
      if (event.eventType === EVENT_TYPES.HypothesisIntegrated && p.mechanicId) {
        await tryRel(graph,
          `MATCH (h:Hypothesis {id: $hid}), (m:Mechanic {id: $mid})
           MERGE (h)-[:INTEGRATES_AS]->(m)`,
          { hid: id, mid: s(p.mechanicId) },
        );
      }
      return;

    default:
      return;
  }
}

/** 関係 MERGE を best-effort で試みる。片方のノードが未存在なら静かにスキップ。 */
async function tryRel(
  graph: KuzuGraphClient,
  cypher: string,
  params: Record<string, KuzuValue>,
): Promise<void> {
  try {
    await graph.run(cypher, params);
  } catch {
    // MATCH が 0 件の場合は MERGE できない。結果整合 OK（rebuild で補完）。
  }
}

/** 全 events を Kuzu に replay する（再構築コマンド用）。 */
export async function replayEventsToKuzu(
  graph: KuzuGraphClient,
  events: DomainEvent[],
): Promise<{ applied: number; failed: number }> {
  let applied = 0;
  let failed = 0;
  for (const event of events) {
    try {
      await applyKuzuProjection(graph, event);
      applied++;
    } catch {
      failed++;
    }
  }
  return { applied, failed };
}
