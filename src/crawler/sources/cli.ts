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
import type { ExternalUtterance } from "./types.js";

const STAGE_DIR = path.resolve("./data/external");

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

function writeJsonl(file: string, items: ExternalUtterance[]): void {
  fs.mkdirSync(path.dirname(file), { recursive: true });
  fs.writeFileSync(file, items.map((i) => JSON.stringify(i)).join("\n") + (items.length ? "\n" : ""), "utf8");
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

/** ext-fetch: collector を回して中間 JSONL に保存する (DB には入れない)。 */
export async function runExtFetch(rest: string[]): Promise<void> {
  const [source, ...sourceArgs] = rest;
  if (source !== "steam") {
    console.error("ext-fetch: only 'steam' is implemented (Phase 1)");
    process.exit(2);
  }
  const args = parseSteamArgs(sourceArgs);
  const items = await fetchSteam(args);
  process.stderr.write("\n");
  const out = stagePath(source, args.gameSlug);
  writeJsonl(out, items);
  console.log(
    JSON.stringify(
      { source, gameSlug: args.gameSlug, appId: args.appId, fetched: items.length, out: path.relative(process.cwd(), out) },
      null,
      2
    )
  );
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
  if (source !== "steam") {
    console.error("ext-ingest: only 'steam' is implemented (Phase 1)");
    process.exit(2);
  }
  const args = parseSteamArgs(sourceArgs);
  const items = await fetchSteam(args);
  process.stderr.write("\n");
  const out = stagePath(source, args.gameSlug);
  writeJsonl(out, items);

  const core = createCore();
  const ingested = openIngestedStore();
  try {
    const result = importExternalUtterances(core, items, {
      workspaceId: getConfig().workspace,
      ingested,
    });
    console.log(
      JSON.stringify(
        {
          source,
          gameSlug: args.gameSlug,
          appId: args.appId,
          fetched: items.length,
          stage: path.relative(process.cwd(), out),
          ...result,
        },
        null,
        2
      )
    );
  } finally {
    ingested.close();
    core.close();
  }
}
