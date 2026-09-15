import { randomUUID } from "node:crypto";
import { setTimeout as sleep } from "node:timers/promises";
import type { HistorySettings, HistoryJob } from "./types.js";
import { HistoryStore } from "./store.js";
import { collectCommentPage, collectLivePage } from "./youtube-pages.js";
import { collectSteamPage } from "./steam-pages.js";
import { SourceError } from "./youtube-client.js";

export interface CollectionResult { requests: number; pending: number; complete: number; unavailable: number; errors: number }

/** One bounded sweep. A scheduler can rerun it; checkpoints survive interruption. */
export async function collectHistory(settings: HistorySettings, {
  clock = Date.now, fetcher = fetch,
}: { clock?: () => number; fetcher?: typeof fetch } = {}): Promise<CollectionResult> {
  const store = new HistoryStore(settings.database);
  const owner = randomUUID();
  let requests = 0;
  try {
    store.acquire(owner, clock());
    store.prune(clock());
    const seed = (kind: HistoryJob["kind"], target: string, topic: string): void => {
      store.seed({ key: `${kind}:${target}`, kind, target, topic, startedAt: clock(), nextAt: clock(), status: "pending" });
    };
    for (const video of settings.videos) {
      if (video.comments) seed("comments", video.id, video.topic);
      if (video.livechat) seed("livechat", video.id, video.topic);
    }
    for (const app of settings.steam) seed("steam", String(app.appId), app.topic);
    const videoTargets = new Map(settings.videos.map(video => [video.id, video]));
    const steamTargets = new Set(settings.steam.map(app => String(app.appId)));
    const selected = (job: HistoryJob): boolean => job.kind === "steam" ? steamTargets.has(job.target)
      : job.kind === "livechat" ? videoTargets.get(job.target)?.livechat === true
      : videoTargets.get(job.videoId ?? job.target)?.comments === true;
    while (requests < settings.maxRequests) {
      const now = clock();
      store.acquire(owner, now);
      const jobs = store.jobs().filter(selected).filter(job => job.nextAt <= now && job.detail !== "live_stream_ended")
        .sort((left, right) => left.nextAt - right.nextAt || left.key.localeCompare(right.key));
      const found = jobs[0];
      if (!found) break;
      const job = found.status === "complete" ? { ...found, status: "pending" as const, cursor: undefined } : found;
      try {
        requests += 1;
        if (job.kind === "steam") {
          await collectSteamPage(job, store, now, fetcher, clock);
          // Also bound requests across different app IDs, not only within one cursor.
          await sleep(1200);
        } else if (job.kind === "livechat") await collectLivePage(job, settings, store, now, fetcher, clock);
        else await collectCommentPage(job, settings, store, now, fetcher, clock);
      } catch (error) {
        const message = error instanceof Error ? error.message : "collection_failed";
        const unavailable = error instanceof SourceError &&
          ["commentsDisabled", "videoNotFound", "liveChatDisabled", "liveChatEnded", "liveChatNotFound"].includes(error.reason);
        const resetLive = error instanceof SourceError && error.reason === "invalidPageToken" && job.kind === "livechat";
        store.save({ ...job, status: unavailable ? "unavailable" : "error", nextAt: now + 3600_000,
          cursor: resetLive ? undefined : job.cursor,
          gaps: resetLive ? [...(job.gaps ?? []), `${new Date(now).toISOString()}: invalidPageToken`] : job.gaps,
          detail: resetLive ? "live_chat_gap: invalidPageToken; next sweep starts a new observation" : message });
        if (message === "youtube_daily_budget_exhausted" ||
          (error instanceof SourceError && ["quotaExceeded", "dailyLimitExceeded", "keyInvalid"].includes(error.reason))) break;
      }
    }
    const jobs = store.jobs().filter(selected);
    return { requests, pending: jobs.filter(j => j.status === "pending").length,
      complete: jobs.filter(j => j.status === "complete").length, unavailable: jobs.filter(j => j.status === "unavailable").length,
      errors: jobs.filter(j => j.status === "error").length };
  } finally { try { store.release(owner); } finally { store.close(); } }
}
