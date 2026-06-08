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
import type { AttributionStore } from "./attribution-store.js";
import type { IngestedStore } from "./ingested-store.js";
import type { RawStore } from "./raw-store.js";
import type { ExternalUtterance } from "./types.js";

type Core = ReturnType<typeof createCore>;

export interface ExternalImportResult {
  sessions: number;
  utterances: number;
  reactions: number;
  skipped: number;
  /** summary を本文に採用し raw を raw-store へ退避した件数 (2 層化、 id=67) */
  summarized: number;
}

export interface ExternalImportOptions {
  workspaceId: string;
  /** dedup sidecar。渡せば取り込み済み (source,nativeId) を skip + 記録する */
  ingested?: IngestedStore;
  /**
   * raw 全文の退避先 (id=67)。 item.summary がある時、 utterance 本文は summary に、
   * raw 全文 (content) はこの store に保存する (トークン節約 2 層化)。 未指定なら raw を本文に。
   */
  rawStore?: RawStore;
  /**
   * 出所メタ sidecar (§3 / §6)。 渡せば utterance 採番直後に source / sourceUrl /
   * nativeId / gameSlug を 1 行 upsert する (全 source。 短文コメントも含む)。
   * 露出 serializer / データ調整 UX (§13) がここから出所を引く。
   */
  attribution?: AttributionStore;
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
  const result: ExternalImportResult = { sessions: 0, utterances: 0, reactions: 0, skipped: 0, summarized: 0 };
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
    // 2 層化: summary があれば本文は要約 (参照される側 = トークン節約)、 raw 全文は退避。
    const useSummary = !!(item.summary && item.summary.trim() && opts.rawStore);
    const bodyContent = useSummary ? item.summary!.trim() : item.content;
    core.repos.utterance.create({
      id: utteranceId,
      workspaceId: ws,
      sessionId,
      speakerId,
      rawContent: bodyContent,
      postedAt: item.postedAt,
      respondsTo,
    });
    if (useSummary) {
      opts.rawStore!.set({ utteranceId, source: item.source, url: item.sourceUrl, rawText: item.content });
      result.summarized += 1;
    }
    // 出所メタを保持 (全 source)。 露出層が個人をマスクしつつ source/sourceUrl を開示する素 (§6)。
    opts.attribution?.set({
      utteranceId,
      source: item.source,
      sourceUrl: item.sourceUrl,
      nativeId: item.nativeId,
      gameSlug: item.gameSlug,
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
