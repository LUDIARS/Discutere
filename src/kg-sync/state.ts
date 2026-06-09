/**
 * follower 側の同期状態 (source URL ごとの透かし) — spec/core/KG-SYNC.md。
 *
 * 取得済みの最大 updated_at を URL ごとに JSON ファイルに保存し、次回 since に渡して
 * 増分のみ取得する。 backup-state.json と同じ単純な JSON ファイル方式。
 */

import { existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import path from "node:path";

export const DEFAULT_SYNC_STATE_PATH = "./data/kg-sync-state.json";

export interface SourceSyncState {
  watermark: number;
  lastPulledAt: number;
  lastApplied: number;
}

export interface KgSyncState {
  sources: Record<string, SourceSyncState>;
}

export function readSyncState(statePath = DEFAULT_SYNC_STATE_PATH): KgSyncState {
  const abs = path.resolve(statePath);
  if (!existsSync(abs)) return { sources: {} };
  try {
    const parsed = JSON.parse(readFileSync(abs, "utf8")) as Partial<KgSyncState>;
    return { sources: parsed.sources ?? {} };
  } catch {
    return { sources: {} };
  }
}

export function writeSyncState(state: KgSyncState, statePath = DEFAULT_SYNC_STATE_PATH): void {
  const abs = path.resolve(statePath);
  mkdirSync(path.dirname(abs), { recursive: true });
  writeFileSync(abs, JSON.stringify(state, null, 2), "utf8");
}

export function getSourceWatermark(state: KgSyncState, url: string): number {
  return state.sources[url]?.watermark ?? 0;
}

export function setSourceState(state: KgSyncState, url: string, next: SourceSyncState): void {
  state.sources[url] = next;
}
