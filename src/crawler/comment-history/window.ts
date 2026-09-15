/** Six calendar months, clamped at month end; all history timestamps are UTC. */
export function historyStart(now: number): number {
  if (!Number.isFinite(now)) throw new Error("Invalid history clock");
  const date = new Date(now);
  const day = date.getUTCDate();
  date.setUTCDate(1);
  date.setUTCMonth(date.getUTCMonth() - 6);
  const last = new Date(Date.UTC(date.getUTCFullYear(), date.getUTCMonth() + 1, 0)).getUTCDate();
  date.setUTCDate(Math.min(day, last));
  return date.getTime();
}

export function youtubeQuotaDay(now: number): string {
  return new Intl.DateTimeFormat("en-CA", {
    timeZone: "America/Los_Angeles", year: "numeric", month: "2-digit", day: "2-digit",
  }).format(new Date(now));
}

export const YOUTUBE_REFRESH_MS = 30 * 24 * 60 * 60 * 1000;
