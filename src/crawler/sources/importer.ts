/**
 * External importer (Phase 1) — spec/crawler/EXTERNAL-SOURCES.md §3.
 *
 * ExternalUtterance[] を Discatier Core の sessions / utterances / reactions に
 * 格納する。決定的 id (`ext:<source>:<nativeId>`) と dedup sidecar で冪等。
 *
 * - session_id   = `ext:<source>:<threadKey>` (議論の場 = 1 session)
 * - utterance.id = `ext:<source>:<nativeId>`  (再 import で重複 INSERT しないよう存在確認)
 * - speaker_id   = `ext:<source>:<authorId>`  (公開 ID アンカー、 露出時に persona へマスク)
 * - responds_to  = `ext:<source>:<parentNativeId>`
 * - signal.votedUp → reactions (positive/negative, intensity=upvotes)
 */

import type { createCore } from "../../core/index.js";

import { toSpeakerId } from "./persona.js";
import type { IngestedStore } from "./ingested-store.js";
import type { ExternalUtterance } from "./types.js";

type Core = ReturnType<typeof createCore>;

export interface ExternalImportResult {
  sessions: number;
  utterances: number;
  reactions: number;
  skipped: number;
}

export interface ExternalImportOptions {
  workspaceId: string;
  /** dedup sidecar。渡せば取り込み済み (source,nativeId) を skip + 記録する */
  ingested?: IngestedStore;
  /** session タイトル生成 (既定 `<source>:<gameSlug>`) */
  sessionTitle?: (item: ExternalUtterance) => string;
}

function extId(source: string, nativeId: string): string {
  return `ext:${source}:${nativeId}`;
}

export function importExternalUtterances(
  core: Core,
  items: ExternalUtterance[],
  opts: ExternalImportOptions
): ExternalImportResult {
  const ws = opts.workspaceId;
  const result: ExternalImportResult = { sessions: 0, utterances: 0, reactions: 0, skipped: 0 };
  const ensuredSessions = new Set<string>();

  // 親→子 (postedAt 昇順) に並べ、responds_to 先が先に入るようにする。
  const ordered = [...items].sort((a, b) => a.postedAt - b.postedAt);

  for (const item of ordered) {
    if (opts.ingested?.has(item.source, item.nativeId)) {
      result.skipped += 1;
      continue;
    }

    const sessionId = extId(item.source, item.threadKey);
    if (!ensuredSessions.has(sessionId)) {
      if (ensureSession(core, ws, sessionId, item, opts)) result.sessions += 1;
      ensuredSessions.add(sessionId);
    }

    const utteranceId = extId(item.source, item.nativeId);
    if (utteranceExists(core, utteranceId)) {
      // 既に DB にある (sidecar 未使用での再実行等) → 記録だけ更新して skip
      opts.ingested?.add(item.source, item.nativeId);
      result.skipped += 1;
      continue;
    }

    const speakerId = toSpeakerId(item.source, item.authorId);
    const respondsTo = item.parentNativeId ? extId(item.source, item.parentNativeId) : undefined;
    core.repos.utterance.create({
      id: utteranceId,
      workspaceId: ws,
      sessionId,
      speakerId,
      rawContent: item.content,
      postedAt: item.postedAt,
      respondsTo,
    });
    result.utterances += 1;

    if (item.signal?.votedUp !== undefined) {
      core.repos.reaction.create({
        id: `${utteranceId}:vote`,
        workspaceId: ws,
        utteranceId,
        actorId: speakerId,
        reactionType: item.signal.votedUp ? "positive" : "negative",
        intensity: item.signal.upvotes,
      });
      result.reactions += 1;
    }

    opts.ingested?.add(item.source, item.nativeId);
  }

  return result;
}

/** session 行が無ければ作る。新規作成したら true。 */
function ensureSession(
  core: Core,
  ws: string,
  sessionId: string,
  item: ExternalUtterance,
  opts: ExternalImportOptions
): boolean {
  const exists = core.client.raw.prepare("SELECT 1 FROM sessions WHERE id = ?").get(sessionId);
  const title = opts.sessionTitle ? opts.sessionTitle(item) : `${item.source}:${item.gameSlug}`;
  core.repos.session.create({
    id: sessionId,
    workspaceId: ws,
    title,
    startedAt: item.postedAt,
    mode: "external",
  });
  return exists === undefined;
}

function utteranceExists(core: Core, id: string): boolean {
  return core.client.raw.prepare("SELECT 1 FROM utterances WHERE id = ?").get(id) !== undefined;
}
