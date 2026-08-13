/**
 * steam-persona 収集の 1 実行 (発見 → 定期取得 → 横断検出 → 集中収集)。
 * 定期実行は Concordia の timer delegation が `npm run steam-persona` を叩く
 * (Di 常駐プロセスには依存しない)。spec/feature/crawler/STEAM-PERSONA.md §2。
 */

import { DEFAULT_AUTHOR_DIR, collectCrossAuthors } from "./collect.js";
import { discoverNewReleases } from "./discover.js";
import type { SteamFetchPorts } from "./ports.js";
import { DEFAULT_STEAM_PERSONA_DB_PATH, openSteamPersonaStore } from "./store.js";
import { trackAppReviews } from "./track.js";

export interface SteamPersonaRunOptions {
  /** 新作一覧から見る件数。既定 200。 */
  maxApps?: number;
  /** 追跡登録するレビュー総数の下限。既定 200 (これを超えるもの)。 */
  minTotalReviews?: number;
  /** 横断判定の最小アプリ数。既定 2。 */
  minCrossApps?: number;
  /** 1 アプリあたりの増分取得上限。既定 2000。 */
  maxReviewsPerApp?: number;
  /** 1 実行で集中収集する投稿者数。既定 10。 */
  maxAuthorsPerRun?: number;
  dbPath?: string;
  outDir?: string;
  now?: () => number;
  log?: (msg: string) => void;
}

export interface SteamPersonaRunSummary {
  scanned: number;
  trackedNew: number;
  trackedTotal: number;
  sightings: number;
  detected: number;
  collectedAuthors: number;
  collectedReviews: number;
}

/** 1 周分のパイプラインを回す。 */
export async function runSteamPersona(
  ports: SteamFetchPorts,
  opts: SteamPersonaRunOptions = {}
): Promise<SteamPersonaRunSummary> {
  const now = opts.now ?? Date.now;
  const log = opts.log;
  const store = openSteamPersonaStore(opts.dbPath ?? DEFAULT_STEAM_PERSONA_DB_PATH);
  try {
    const discover = await discoverNewReleases(ports, store, {
      maxApps: opts.maxApps ?? 200,
      minTotalReviews: opts.minTotalReviews ?? 200,
      now,
      log,
    });
    const track = await trackAppReviews(ports, store, {
      maxReviewsPerApp: opts.maxReviewsPerApp ?? 2000,
      now,
      log,
    });
    const detected = store.detectCrossAuthors(opts.minCrossApps ?? 2, now());
    for (const a of detected) log?.(`detected cross author ${a.steamId} (${a.appCount} apps)`);
    const collect = await collectCrossAuthors(ports, store, {
      maxAuthorsPerRun: opts.maxAuthorsPerRun ?? 10,
      outDir: opts.outDir ?? DEFAULT_AUTHOR_DIR,
      now,
      log,
    });
    return {
      scanned: discover.scanned,
      trackedNew: discover.tracked,
      trackedTotal: store.listApps().length,
      sightings: track.sightings,
      detected: detected.length,
      collectedAuthors: collect.authors,
      collectedReviews: collect.reviews,
    };
  } finally {
    store.close();
  }
}
