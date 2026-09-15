import type { ExternalUtterance } from "../sources/types.js";

export type HistorySource = "youtube" | "youtube-livechat" | "steam";
export type JobKind = "comments" | "replies" | "livechat" | "steam";
export interface HistoryJob {
  key: string;
  kind: JobKind;
  target: string;
  topic: string;
  videoId?: string;
  cursor?: string;
  liveChatId?: string;
  startedAt: number;
  nextAt: number;
  status: "pending" | "complete" | "unavailable" | "error";
  detail?: string;
  gaps?: string[];
}
export interface HistoryRecord extends ExternalUtterance {
  source: HistorySource;
  fetchedAt: number;
  expiresAt: number;
}
export interface HistorySnapshot {
  schemaVersion: 1;
  generatedAt: number;
  windowStart: number;
  expiresAt: number;
  coverage: HistoryJob[];
  records: HistoryRecord[];
}
export interface HistorySettings {
  database: string;
  youtubeApiKey?: string;
  youtubeDailyUnits: number;
  liveChatRequestUnits: number;
  youtubeRetentionDays: number;
  youtubeApprovalReference?: string;
  maxRequests: number;
  videos: Array<{ id: string; topic: string; comments: boolean; livechat: boolean }>;
  steam: Array<{ appId: number; topic: string }>;
}
