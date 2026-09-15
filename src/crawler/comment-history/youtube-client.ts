import type { HistoryStore } from "./store.js";
import type { HistorySettings } from "./types.js";

export class SourceError extends Error {
  constructor(readonly reason: string, readonly status: number) { super(`YouTube ${status}: ${reason}`); }
}

/** Reserve before sending: failed and uncertain requests still consume quota. */
export async function youtubeRequest<T>(
  route: string, parameters: Record<string, string>, settings: HistorySettings, store: HistoryStore,
  now: number, fetcher: typeof fetch,
): Promise<T> {
  if (!settings.youtubeApiKey) throw new Error("YOUTUBE_API_KEY is required");
  store.reserveQuota(now, route === "liveChat/messages" ? settings.liveChatRequestUnits : 1, settings.youtubeDailyUnits);
  const url = new URL(`https://www.googleapis.com/youtube/v3/${route}`);
  for (const [key, value] of Object.entries(parameters)) if (value) url.searchParams.set(key, value);
  url.searchParams.set("key", settings.youtubeApiKey);
  let response: Response;
  try { response = await fetcher(url, { signal: AbortSignal.timeout(30_000), redirect: "error" }); }
  catch { throw new Error("YouTube request failed; quota reservation retained"); }
  if (!response.ok) {
    const error = await response.json().catch(() => null) as { error?: { errors?: Array<{ reason?: string }> } } | null;
    const reason = error?.error?.errors?.[0]?.reason;
    // Never reflect a response body or request URL containing the API key.
    throw new SourceError(reason && /^[A-Za-z]+$/.test(reason) ? reason : "requestFailed", response.status);
  }
  return await response.json() as T;
}
