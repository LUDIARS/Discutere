/**
 * External sources CLI ハンドラ — scripts/crawl.ts から呼ばれる (Phase 1)。
 *
 *   ext-fetch  steam <gameSlug> <appId> [--lang all|japanese] [--max N]
 *   ext-import <jsonl-path>
 *   ext-ingest steam <gameSlug> <appId> [...] (fetch → 中間 JSONL → import を一括)
 */

import fs from "node:fs";
import path from "node:path";

import { getConfig } from "../../config.js";
import { createCore } from "../../core/index.js";

import { importExternalUtterances } from "./importer.js";
import { openIngestedStore } from "./ingested-store.js";
import { fetchSteamReviews } from "./steam.js";
import { fetchWebsiteArticles } from "./website.js";
import { fetchRedditDiscussions, type RedditCredentials } from "./reddit.js";
import { fetchVideoComments } from "./youtube-comments.js";
import { createQuotaTracker } from "./youtube-quota.js";
import { discoverVideosBySearch, type VideoRef } from "./youtube-videos.js";
import type { ExternalUtterance } from "./types.js";

const STAGE_DIR = path.resolve("./data/external");

function youtubeApiKey(): string {
  const key = process.env.DISCUTERE_YOUTUBE_API_KEY;
  if (!key) {
    console.error("YouTube API key 未設定: env DISCUTERE_YOUTUBE_API_KEY をセットしてください");
    process.exit(2);
  }
  return key;
}

function redditCredentials(): RedditCredentials {
  const clientId = process.env.DISCUTERE_REDDIT_CLIENT_ID;
  const clientSecret = process.env.DISCUTERE_REDDIT_CLIENT_SECRET;
  if (!clientId || !clientSecret) {
    console.error(
      "Reddit 認証未設定: env DISCUTERE_REDDIT_CLIENT_ID / DISCUTERE_REDDIT_CLIENT_SECRET をセットしてください"
    );
    process.exit(2);
  }
  const userAgent =
    process.env.DISCUTERE_REDDIT_USER_AGENT ?? "LUDIARS-Discutere/0.1 (external discussion crawler)";
  return { clientId, clientSecret, userAgent };
}

interface RedditArgs {
  gameSlug: string;
  query: string;
  subreddit?: string;
  maxThreads?: number;
}

function parseRedditArgs(rest: string[]): RedditArgs {
  const [gameSlug, ...flags] = rest;
  let query: string | undefined;
  let subreddit: string | undefined;
  let maxThreads: number | undefined;
  for (let i = 0; i < flags.length; i += 1) {
    if (flags[i] === "--q" || flags[i] === "--query") query = flags[++i];
    else if (flags[i] === "--sub" || flags[i] === "--subreddit") subreddit = flags[++i];
    else if (flags[i] === "--threads") maxThreads = Number(flags[++i]);
  }
  if (!gameSlug || !query) {
    console.error('usage: crawl.ts ext-fetch reddit <gameSlug> --q "<query>" [--sub <subreddit>] [--threads N]');
    process.exit(2);
  }
  return { gameSlug, query, subreddit, maxThreads };
}

async function fetchReddit(args: RedditArgs): Promise<ExternalUtterance[]> {
  return fetchRedditDiscussions({
    ...redditCredentials(),
    gameSlug: args.gameSlug,
    query: args.query,
    subreddit: args.subreddit,
    maxThreads: args.maxThreads,
    onThread: (i) => process.stderr.write(`\r  reddit: ${i.total} comments (${i.comments} from latest thread)...`),
  });
}

interface FlagOpts {
  query?: string;
  maxItems?: number;
}

function parseFlags(flags: string[]): FlagOpts {
  const out: FlagOpts = {};
  for (let i = 0; i < flags.length; i += 1) {
    if (flags[i] === "--q" || flags[i] === "--query") out.query = flags[++i];
    else if (flags[i] === "--max") out.maxItems = Number(flags[++i]);
  }
  return out;
}

interface SteamArgs {
  gameSlug: string;
  appId: number;
  languages?: string[];
  maxReviews?: number;
}

function parseSteamArgs(rest: string[]): SteamArgs {
  const [gameSlug, appIdArg, ...flags] = rest;
  if (!gameSlug || !appIdArg) {
    console.error("usage: crawl.ts ext-fetch steam <gameSlug> <appId> [--lang all] [--max N]");
    process.exit(2);
  }
  const appId = Number(appIdArg);
  if (!Number.isFinite(appId)) {
    console.error(`invalid appId: ${appIdArg}`);
    process.exit(2);
  }
  let languages: string[] | undefined;
  let maxReviews: number | undefined;
  for (let i = 0; i < flags.length; i += 1) {
    if (flags[i] === "--lang") languages = flags[++i]?.split(",").map((s) => s.trim());
    else if (flags[i] === "--max") maxReviews = Number(flags[++i]);
  }
  return { gameSlug, appId, languages, maxReviews };
}

function stagePath(source: string, gameSlug: string): string {
  return path.join(STAGE_DIR, source, `${gameSlug}.jsonl`);
}

function writeJsonl(file: string, items: readonly unknown[]): void {
  fs.mkdirSync(path.dirname(file), { recursive: true });
  fs.writeFileSync(file, items.map((i) => JSON.stringify(i)).join("\n") + (items.length ? "\n" : ""), "utf8");
}

function ytStagePath(gameSlug: string, videoId: string): string {
  return path.join(STAGE_DIR, "youtube", gameSlug, `${videoId}.jsonl`);
}

/** ExternalUtterance[] を Discatier Core に取り込み、結果を返す。 */
function importItems(items: ExternalUtterance[]) {
  const core = createCore();
  const ingested = openIngestedStore();
  try {
    return importExternalUtterances(core, items, { workspaceId: getConfig().workspace, ingested });
  } finally {
    ingested.close();
    core.close();
  }
}

function readJsonl(file: string): ExternalUtterance[] {
  return fs
    .readFileSync(file, "utf8")
    .split("\n")
    .map((l) => l.trim())
    .filter((l) => l.length > 0)
    .map((l) => JSON.parse(l) as ExternalUtterance);
}

async function fetchSteam(args: SteamArgs): Promise<ExternalUtterance[]> {
  return fetchSteamReviews({
    appId: args.appId,
    gameSlug: args.gameSlug,
    languages: args.languages,
    maxReviews: args.maxReviews,
    onPage: (n) => process.stderr.write(`\r  fetched ${n} reviews...`),
  });
}

async function fetchYoutubeCommentsFor(
  gameSlug: string,
  videoId: string,
  maxComments?: number
): Promise<ExternalUtterance[]> {
  return fetchVideoComments({
    videoId,
    gameSlug,
    apiKey: youtubeApiKey(),
    maxComments,
    quota: createQuotaTracker(),
    onPage: (n) => process.stderr.write(`\r  fetched ${n} comments...`),
  });
}

interface WebsiteArgs {
  gameSlug: string;
  urls: string[];
}

function parseWebsiteArgs(rest: string[]): WebsiteArgs {
  const [gameSlug, ...urls] = rest;
  if (!gameSlug || urls.length === 0) {
    console.error("usage: crawl.ts ext-fetch website <gameSlug> <url> [<url> ...]");
    process.exit(2);
  }
  return { gameSlug, urls };
}

async function fetchWebsite(args: WebsiteArgs): Promise<ExternalUtterance[]> {
  return fetchWebsiteArticles({
    urls: args.urls,
    gameSlug: args.gameSlug,
    onPage: (n) => process.stderr.write(`\r  fetched ${n}/${args.urls.length} articles...`),
  });
}

/** ext-fetch: collector を回して中間 JSONL に保存する (DB には入れない)。 */
export async function runExtFetch(rest: string[]): Promise<void> {
  const [source, ...sourceArgs] = rest;
  switch (source) {
    case "steam": {
      const args = parseSteamArgs(sourceArgs);
      const items = await fetchSteam(args);
      process.stderr.write("\n");
      const out = stagePath(source, args.gameSlug);
      writeJsonl(out, items);
      console.log(JSON.stringify({ source, gameSlug: args.gameSlug, appId: args.appId, fetched: items.length, out: path.relative(process.cwd(), out) }, null, 2));
      return;
    }
    case "youtube-videos": {
      const [gameSlug, ...flags] = sourceArgs;
      const { query, maxItems } = parseFlags(flags);
      if (!gameSlug || !query) {
        console.error('usage: crawl.ts ext-fetch youtube-videos <gameSlug> --q "<query>" [--max N]');
        process.exit(2);
      }
      const videos = await discoverVideosBySearch({ query, apiKey: youtubeApiKey(), maxResults: maxItems ?? 50 });
      const out = path.join(STAGE_DIR, "youtube", "videos", `${gameSlug}.jsonl`);
      writeJsonl(out, videos as VideoRef[]);
      console.log(JSON.stringify({ source, gameSlug, query, videos: videos.length, out: path.relative(process.cwd(), out) }, null, 2));
      return;
    }
    case "youtube-comments": {
      const [gameSlug, videoId, ...flags] = sourceArgs;
      if (!gameSlug || !videoId) {
        console.error("usage: crawl.ts ext-fetch youtube-comments <gameSlug> <videoId> [--max N]");
        process.exit(2);
      }
      const { maxItems } = parseFlags(flags);
      const items = await fetchYoutubeCommentsFor(gameSlug, videoId, maxItems);
      process.stderr.write("\n");
      const out = ytStagePath(gameSlug, videoId);
      writeJsonl(out, items);
      console.log(JSON.stringify({ source, gameSlug, videoId, fetched: items.length, out: path.relative(process.cwd(), out) }, null, 2));
      return;
    }
    case "website": {
      const args = parseWebsiteArgs(sourceArgs);
      const items = await fetchWebsite(args);
      process.stderr.write("\n");
      const out = stagePath(source, args.gameSlug);
      writeJsonl(out, items);
      console.log(JSON.stringify({ source, gameSlug: args.gameSlug, urls: args.urls.length, fetched: items.length, out: path.relative(process.cwd(), out) }, null, 2));
      return;
    }
    case "reddit": {
      const args = parseRedditArgs(sourceArgs);
      const items = await fetchReddit(args);
      process.stderr.write("\n");
      const out = stagePath(source, args.gameSlug);
      writeJsonl(out, items);
      console.log(JSON.stringify({ source, gameSlug: args.gameSlug, query: args.query, fetched: items.length, out: path.relative(process.cwd(), out) }, null, 2));
      return;
    }
    default:
      console.error("ext-fetch: source は steam | youtube-videos | youtube-comments | website | reddit");
      process.exit(2);
  }
}

/** ext-import: 中間 JSONL → Discatier Core (dedup + persona アンカー)。 */
export function runExtImport(rest: string[]): void {
  const [jsonlPath] = rest;
  if (!jsonlPath) {
    console.error("usage: crawl.ts ext-import <jsonl-path>");
    process.exit(2);
  }
  const abs = path.resolve(jsonlPath);
  if (!fs.existsSync(abs)) {
    console.error(`jsonl not found: ${abs}`);
    process.exit(1);
  }
  const items = readJsonl(abs);
  const core = createCore();
  const ingested = openIngestedStore();
  try {
    const result = importExternalUtterances(core, items, {
      workspaceId: getConfig().workspace,
      ingested,
    });
    console.log(
      JSON.stringify({ file: path.relative(process.cwd(), abs), read: items.length, ...result }, null, 2)
    );
  } finally {
    ingested.close();
    core.close();
  }
}

/** ext-ingest: fetch → 中間 JSONL 保存 → import を一括 (取得して格納)。 */
export async function runExtIngest(rest: string[]): Promise<void> {
  const [source, ...sourceArgs] = rest;
  switch (source) {
    case "steam": {
      const args = parseSteamArgs(sourceArgs);
      const items = await fetchSteam(args);
      process.stderr.write("\n");
      const out = stagePath(source, args.gameSlug);
      writeJsonl(out, items);
      const result = importItems(items);
      console.log(JSON.stringify({ source, gameSlug: args.gameSlug, appId: args.appId, fetched: items.length, stage: path.relative(process.cwd(), out), ...result }, null, 2));
      return;
    }
    case "youtube": {
      const [gameSlug, videoId, ...flags] = sourceArgs;
      if (!gameSlug || !videoId) {
        console.error("usage: crawl.ts ext-ingest youtube <gameSlug> <videoId> [--max N]");
        process.exit(2);
      }
      const { maxItems } = parseFlags(flags);
      const items = await fetchYoutubeCommentsFor(gameSlug, videoId, maxItems);
      process.stderr.write("\n");
      const out = ytStagePath(gameSlug, videoId);
      writeJsonl(out, items);
      const result = importItems(items);
      console.log(JSON.stringify({ source, gameSlug, videoId, fetched: items.length, stage: path.relative(process.cwd(), out), ...result }, null, 2));
      return;
    }
    case "website": {
      const args = parseWebsiteArgs(sourceArgs);
      const items = await fetchWebsite(args);
      process.stderr.write("\n");
      const out = stagePath(source, args.gameSlug);
      writeJsonl(out, items);
      const result = importItems(items);
      console.log(JSON.stringify({ source, gameSlug: args.gameSlug, urls: args.urls.length, fetched: items.length, stage: path.relative(process.cwd(), out), ...result }, null, 2));
      return;
    }
    case "reddit": {
      const args = parseRedditArgs(sourceArgs);
      const items = await fetchReddit(args);
      process.stderr.write("\n");
      const out = stagePath(source, args.gameSlug);
      writeJsonl(out, items);
      const result = importItems(items);
      console.log(JSON.stringify({ source, gameSlug: args.gameSlug, query: args.query, fetched: items.length, stage: path.relative(process.cwd(), out), ...result }, null, 2));
      return;
    }
    default:
      console.error("ext-ingest: source は steam | youtube | website | reddit");
      process.exit(2);
  }
}
