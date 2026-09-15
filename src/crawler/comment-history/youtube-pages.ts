import { mapYoutubeComment, type CommentThreadRaw, type YoutubeCommentRaw } from "../sources/youtube-comments.js";
import type { HistoryStore } from "./store.js";
import type { HistoryJob, HistoryRecord, HistorySettings } from "./types.js";
import { historyStart } from "./window.js";
import { youtubeRequest } from "./youtube-client.js";

interface Page<T> { items?: T[]; nextPageToken?: string }
const DAY = 86400_000;

function comment(raw: YoutubeCommentRaw, job: HistoryJob, now: number, retentionDays: number, parent?: string): HistoryRecord[] {
  const row = mapYoutubeComment(raw, job.videoId ?? job.target, job.topic, parent);
  if (!row.nativeId || row.authorId === "unknown" || !row.authorId || !row.content.trim() ||
      !Number.isFinite(row.postedAt) || row.postedAt < historyStart(now) || row.postedAt > now) return [];
  // The snapshot carries stable IDs internally, never display names.
  delete row.authorName;
  return [{ ...row, source: "youtube", fetchedAt: now, expiresAt: now + retentionDays * DAY }];
}

export async function collectCommentPage(job: HistoryJob, settings: HistorySettings, store: HistoryStore,
  now: number, fetcher: typeof fetch, clock: () => number = () => now): Promise<void> {
  const records: HistoryRecord[] = [];
  const children: HistoryJob[] = [];
  let cursor: string | undefined;
  if (job.kind === "replies") {
    const page = await youtubeRequest<Page<YoutubeCommentRaw>>("comments", {
      part: "snippet", parentId: job.target, maxResults: "100", textFormat: "plainText", pageToken: job.cursor ?? "",
    }, settings, store, now, fetcher);
    now = clock();
    for (const item of page.items ?? []) records.push(...comment(item, job, now, settings.youtubeRetentionDays, job.target));
    cursor = page.nextPageToken;
  } else {
    const page = await youtubeRequest<Page<CommentThreadRaw>>("commentThreads", {
      part: "snippet", videoId: job.target, order: "time", maxResults: "100", textFormat: "plainText", pageToken: job.cursor ?? "",
    }, settings, store, now, fetcher);
    now = clock();
    for (const item of page.items ?? []) {
      const top = item.snippet?.topLevelComment;
      if (!top?.id) continue;
      records.push(...comment(top, job, now, settings.youtubeRetentionDays));
      // Old parent comments may have recent replies: never stop at the first old parent.
      if ((item.snippet?.totalReplyCount ?? 0) > 0) children.push({
        key: `replies:${job.target}:${top.id}`, kind: "replies", target: top.id, videoId: job.target,
        topic: job.topic, startedAt: job.startedAt, nextAt: now, status: "pending",
      });
    }
    cursor = page.nextPageToken;
  }
  if (cursor && cursor === job.cursor) throw new Error("youtube_repeated_cursor");
  store.save({ ...job, cursor, status: cursor ? "pending" : "complete", nextAt: cursor ? now : now + DAY,
    detail: cursor ? "pagination_incomplete" : "target_pages_observed" }, records, children);
}

interface LiveMessage {
  id?: string;
  authorDetails?: { channelId?: string };
  snippet?: { type?: string; authorChannelId?: string; publishedAt?: string; displayMessage?: string;
    messageDeletedDetails?: { deletedMessageId?: string } };
}

export async function collectLivePage(job: HistoryJob, settings: HistorySettings, store: HistoryStore,
  now: number, fetcher: typeof fetch, clock: () => number = () => now): Promise<void> {
  if (!job.liveChatId) {
    const page = await youtubeRequest<Page<{ liveStreamingDetails?: { activeLiveChatId?: string } }>>("videos", {
      part: "liveStreamingDetails", id: job.target,
    }, settings, store, now, fetcher);
    now = clock();
    const liveChatId = page.items?.[0]?.liveStreamingDetails?.activeLiveChatId;
    store.save({ ...job, liveChatId, status: liveChatId ? "pending" : "unavailable",
      nextAt: liveChatId ? now : now + 60_000, detail: liveChatId ? "awaiting_first_page" : "no_active_live_chat" });
    return;
  }
  const page = await youtubeRequest<Page<LiveMessage> & { pollingIntervalMillis?: number; offlineAt?: string }>(
    "liveChat/messages", { part: "snippet,authorDetails", liveChatId: job.liveChatId, maxResults: "2000", pageToken: job.cursor ?? "" },
    settings, store, now, fetcher);
  now = clock();
  if (!page.offlineAt && (!Number.isFinite(page.pollingIntervalMillis) || (page.pollingIntervalMillis ?? 0) <= 0)) {
    throw new Error("youtube_invalid_polling_interval");
  }
  if (!page.offlineAt && !page.nextPageToken) throw new Error("youtube_missing_live_cursor");
  const rows: HistoryRecord[] = [];
  const deleted: string[] = [];
  for (const item of page.items ?? []) {
    const snippet = item.snippet;
    if (snippet?.messageDeletedDetails?.deletedMessageId) deleted.push(snippet.messageDeletedDetails.deletedMessageId);
    const authorId = item.authorDetails?.channelId ?? snippet?.authorChannelId;
    const postedAt = Date.parse(snippet?.publishedAt ?? "");
    if (!item.id || !authorId || !snippet?.displayMessage?.trim() ||
        !["textMessageEvent", "superChatEvent"].includes(snippet.type ?? "") ||
        !Number.isFinite(postedAt) || postedAt < Math.max(job.startedAt, historyStart(now)) || postedAt > now) continue;
    rows.push({ source: "youtube-livechat", nativeId: item.id, authorId, postedAt, fetchedAt: now,
      expiresAt: now + settings.youtubeRetentionDays * DAY,
      content: snippet.displayMessage, gameSlug: job.topic, threadKey: job.target,
      sourceUrl: `https://www.youtube.com/watch?v=${job.target}` });
  }
  store.save({ ...job, cursor: page.nextPageToken, status: page.offlineAt ? "complete" : "pending",
    nextAt: page.offlineAt ? now + DAY : now + Math.max(1000, page.pollingIntervalMillis ?? 0),
    detail: page.offlineAt ? "live_stream_ended" : "prospective_only; pre-start history unavailable" }, rows, [], deleted);
}
