/**
 * 共有 KG の pull オーケストレーション (follower 側) — spec/core/KG-SYNC.md。
 *
 * 設定 URL に `?since=<watermark>` を付けて GET → NDJSON を import → 透かしを保存。
 * URL は master の生エンドポイント / 静的 publish ファイルのどちらでも良い (どちらも
 * GET → NDJSON。 静的ホストは since を無視して全件返すが、import 側 LWW で冪等)。
 */

import type Database from "better-sqlite3";

import { importSharedKg } from "./import.js";
import {
  DEFAULT_SYNC_STATE_PATH,
  getSourceWatermark,
  readSyncState,
  setSourceState,
  writeSyncState,
} from "./state.js";

export interface KgPullOptions {
  url: string;
  /** 同期状態ファイル (既定 ./data/kg-sync-state.json)。 */
  statePath?: string;
  /** 配信元が要求する共有シークレット (任意)。?token= と x-kg-token に載せる。 */
  token?: string;
  /** 透かしを無視して全件取り直す (既定 false)。 */
  full?: boolean;
  /** fetch タイムアウト ms (既定 30s)。 */
  timeoutMs?: number;
}

export interface KgPullResult {
  url: string;
  fromWatermark: number;
  toWatermark: number;
  applied: number;
  skipped: number;
  ignored: number;
}

export async function pullSharedKg(db: Database.Database, opts: KgPullOptions): Promise<KgPullResult> {
  const statePath = opts.statePath ?? DEFAULT_SYNC_STATE_PATH;
  const state = readSyncState(statePath);
  const from = opts.full ? 0 : getSourceWatermark(state, opts.url);

  const u = new URL(opts.url);
  u.searchParams.set("since", String(from));
  if (opts.token) u.searchParams.set("token", opts.token);

  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), opts.timeoutMs ?? 30_000);
  let ndjson: string;
  try {
    const res = await fetch(u.toString(), {
      headers: opts.token ? { "x-kg-token": opts.token } : {},
      signal: controller.signal,
    });
    if (!res.ok) throw new Error(`KG pull failed: HTTP ${res.status} ${res.statusText}`);
    ndjson = await res.text();
  } finally {
    clearTimeout(timer);
  }

  const imported = importSharedKg(db, ndjson);
  const to = Math.max(from, imported.watermark);
  setSourceState(state, opts.url, { watermark: to, lastPulledAt: Date.now(), lastApplied: imported.applied });
  writeSyncState(state, statePath);

  return {
    url: opts.url,
    fromWatermark: from,
    toWatermark: to,
    applied: imported.applied,
    skipped: imported.skipped,
    ignored: imported.ignored,
  };
}
