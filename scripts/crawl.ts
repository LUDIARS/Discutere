/**
 * Crawler CLI (Phase 0)
 *
 *   npx tsx scripts/crawl.ts import <md-path>     # md → Discatier Core
 *   npx tsx scripts/crawl.ts list                 # Game / Mechanic / Aesthetic 一覧
 *   npx tsx scripts/crawl.ts run <gameName>       # (Phase 1+)
 *
 * DB path は env DISCATIER_KUZU_PATH > "./data/discatier.kuzu" の順。
 */

import fs from "node:fs";
import path from "node:path";

import { createCore } from "../src/core/index.js";
import { importGameKG, parseGameKG } from "../src/crawler/index.js";

function main(): void {
  const [, , subcommand, ...rest] = process.argv;
  if (!subcommand) {
    printUsageAndExit();
  }

  switch (subcommand) {
    case "import":
      return runImport(rest);
    case "list":
      return runList();
    case "run":
      throw new Error(
        "`run` is not implemented in Phase 0. Author data/games/<slug>.md manually."
      );
    default:
      printUsageAndExit();
  }
}

function runImport(args: string[]): void {
  const [mdPath] = args;
  if (!mdPath) {
    console.error("usage: crawl.ts import <md-path>");
    process.exit(2);
  }
  const abs = path.resolve(mdPath);
  if (!fs.existsSync(abs)) {
    console.error(`md not found: ${abs}`);
    process.exit(1);
  }
  const markdown = fs.readFileSync(abs, "utf8");
  const kg = parseGameKG(markdown, abs);

  const core = createCore();
  try {
    const result = importGameKG(core, kg);
    console.log(
      JSON.stringify(
        {
          file: path.relative(process.cwd(), abs),
          game: { id: result.gameId, title: kg.title },
          inserted: result.inserted,
          updated: result.updated,
        },
        null,
        2
      )
    );
  } finally {
    core.close();
  }
}

function runList(): void {
  const core = createCore();
  try {
    const games = core.client.raw
      .prepare("SELECT id, title, genre FROM games ORDER BY title")
      .all() as Array<{ id: string; title: string; genre: string | null }>;
    if (games.length === 0) {
      console.log("(no games registered)");
      return;
    }
    for (const g of games) {
      const mechanics = core.client.raw
        .prepare("SELECT name FROM mechanics WHERE game_id = ? ORDER BY name")
        .all(g.id) as Array<{ name: string }>;
      const aesthetics = core.client.raw
        .prepare("SELECT name FROM aesthetics WHERE game_id = ? ORDER BY name")
        .all(g.id) as Array<{ name: string }>;
      console.log(`- ${g.title} [${g.genre ?? "?"}]`);
      console.log(`    mechanics:  ${mechanics.map((m) => m.name).join(", ") || "(none)"}`);
      console.log(`    aesthetics: ${aesthetics.map((a) => a.name).join(", ") || "(none)"}`);
    }
  } finally {
    core.close();
  }
}

function printUsageAndExit(): never {
  console.error(
    "usage:\n  crawl.ts import <md-path>\n  crawl.ts list\n  crawl.ts run <gameName>   (Phase 1+)"
  );
  process.exit(2);
}

main();
