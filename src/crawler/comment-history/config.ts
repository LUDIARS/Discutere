import { readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { z } from "zod";
import type { HistorySettings } from "./types.js";

const topic = z.string().trim().min(1).max(120);
const schema = z.object({
  database: z.string().min(1),
  // This is a reserved sub-budget, not a claim about unused Cloud Console quota.
  youtubeDailyUnits: z.number().int().min(1).max(10_000).default(8000),
  // Keep live cost explicit: verify the effective project quota before enabling it.
  liveChatRequestUnits: z.number().int().min(1).max(10_000).optional(),
  youtubeRetentionDays: z.number().int().min(1).max(186).default(30),
  youtubeApprovalReference: z.string().trim().min(1).max(500).optional(),
  maxRequests: z.number().int().min(1).max(1000).default(100),
  videos: z.array(z.object({ id: z.string().regex(/^[A-Za-z0-9_-]{11}$/), topic,
    comments: z.boolean().default(true), livechat: z.boolean().default(false) })).default([]),
  steam: z.array(z.object({ appId: z.number().int().positive(), topic })).default([]),
}).strict();

export function loadHistorySettings(path: string, env: NodeJS.ProcessEnv = process.env, requireKey = true): HistorySettings {
  const value = schema.parse(JSON.parse(readFileSync(path, "utf8")));
  if (!value.steam.length && !value.videos.length) throw new Error("At least one collection target is required");
  if (value.videos.some(video => !video.comments && !video.livechat)) throw new Error("Video target has no enabled source");
  if (new Set(value.videos.map(v => v.id)).size !== value.videos.length ||
      new Set(value.steam.map(v => v.appId)).size !== value.steam.length) throw new Error("Duplicate collection targets");
  if (requireKey && value.videos.length && !env.YOUTUBE_API_KEY?.trim()) throw new Error("YOUTUBE_API_KEY is required");
  if (value.youtubeRetentionDays > 30 && !value.youtubeApprovalReference) {
    throw new Error("YouTube retention beyond 30 days requires an approval reference covering extended storage");
  }
  if (value.videos.some(v => v.livechat) && value.liveChatRequestUnits === undefined) {
    throw new Error("Set liveChatRequestUnits from the effective YouTube project quota before enabling live chat");
  }
  return { ...value, database: resolve(dirname(path), value.database), youtubeApiKey: env.YOUTUBE_API_KEY,
    liveChatRequestUnits: value.liveChatRequestUnits ?? 1 };
}
