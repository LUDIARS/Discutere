/**
 * steam-persona 収集 CLI — 新作レビュー定期取得 → 横断投稿者検出 → 集中収集。
 *
 *   npm run steam-persona [-- --max-apps N --min-total-reviews N --min-cross-apps N
 *                             --max-reviews-per-app N --max-authors N]
 *
 * 定期実行は Concordia timer delegation からこのコマンドを叩く。
 * 取得層は @ludiars/canalis/steam (①adapter)、制御・状態はこのリポ (②) が持つ。
 */

import {
  fetchAppReviewSummary,
  fetchAppReviews,
  fetchNewReleases,
  fetchUserReviews,
} from "@ludiars/canalis/steam";

import type { SteamFetchPorts } from "../src/crawler/steam-persona/ports.js";
import { runSteamPersona, type SteamPersonaRunOptions } from "../src/crawler/steam-persona/runner.js";

function numFlag(args: string[], name: string): number | undefined {
  const i = args.indexOf(name);
  if (i < 0) return undefined;
  const v = Number(args[i + 1]);
  if (!Number.isFinite(v) || v < 0) {
    console.error(`invalid value for ${name}: ${args[i + 1]}`);
    process.exit(2);
  }
  return v;
}

const ports: SteamFetchPorts = {
  fetchNewReleases: ({ maxApps }) => fetchNewReleases({ maxApps }),
  fetchTotalReviews: async (appId) => (await fetchAppReviewSummary({ appId })).total_reviews ?? 0,
  fetchAppReviews: ({ appId, maxReviews, stopAtRecommendationId }) =>
    fetchAppReviews({ appId, maxReviews, stopAtRecommendationId }),
  fetchUserReviews: ({ steamId }) => fetchUserReviews({ steamId }),
};

async function main(): Promise<void> {
  const args = process.argv.slice(2);
  const opts: SteamPersonaRunOptions = {
    maxApps: numFlag(args, "--max-apps"),
    minTotalReviews: numFlag(args, "--min-total-reviews"),
    minCrossApps: numFlag(args, "--min-cross-apps"),
    maxReviewsPerApp: numFlag(args, "--max-reviews-per-app"),
    maxAuthorsPerRun: numFlag(args, "--max-authors"),
    log: (msg) => process.stderr.write(`  ${msg}\n`),
  };
  const summary = await runSteamPersona(ports, opts);
  console.log(JSON.stringify(summary, null, 2));
}

main().catch((err) => {
  console.error(err instanceof Error ? err.message : String(err));
  process.exit(1);
});
