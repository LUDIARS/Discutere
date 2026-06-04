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
import {
  createAiTrainingRunner,
  importGameKG,
  parseGameKG,
} from "../src/crawler/index.js";
import { runExtFetch, runExtImport, runExtIngest } from "../src/crawler/sources/cli.js";
import {
  AnthropicSdkClient,
  ClaudeCliClient,
  type LLMClient,
} from "../src/persona-engine/index.js";
import { getConfig } from "../src/config.js";

async function main(): Promise<void> {
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
      return void runAi(rest).catch((err) => {
        console.error((err as Error).message);
        process.exit(1);
      });
    // 外部議論ソース (Phase 1) — spec/crawler/EXTERNAL-SOURCES.md
    case "ext-fetch":
      return runExtFetch(rest);
    case "ext-import":
      return runExtImport(rest);
    case "ext-ingest":
      return runExtIngest(rest);
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

async function runAi(args: string[]): Promise<void> {
  const [gameName, outArg] = args;
  if (!gameName) {
    console.error("usage: crawl.ts run <gameName> [out-md-path]");
    process.exit(2);
  }

  const config = getConfig();
  const llm = createConfiguredLlm(config);
  const runner = createAiTrainingRunner({
    llm,
    workspaceId: config.workspace,
    model: config.llm.model,
    timeoutMs: config.llm.claudeCliTimeoutMs,
  });
  const result = await runner.run(gameName, { maxSources: 5 });
  const outPath = path.resolve(outArg ?? path.join("data", "games", `${toSlug(result.kg.id || gameName)}.md`));
  fs.mkdirSync(path.dirname(outPath), { recursive: true });
  fs.writeFileSync(outPath, result.markdown, "utf8");

  const core = createCore();
  try {
    const imported = importGameKG(core, result.kg);
    console.log(
      JSON.stringify(
        {
          file: path.relative(process.cwd(), outPath),
          game: { id: imported.gameId, title: result.kg.title },
          inserted: imported.inserted,
          updated: imported.updated,
          meta: result.meta,
        },
        null,
        2
      )
    );
  } finally {
    core.close();
  }
}

function createConfiguredLlm(config: ReturnType<typeof getConfig>): LLMClient {
  if (config.llm.backend === "claude-cli") {
    return new ClaudeCliClient({
      defaultTimeoutMs: config.llm.claudeCliTimeoutMs,
      defaultModel: config.llm.model,
      gitBashPath: config.llm.gitBashPath,
    });
  }
  if (config.llm.anthropicApiKey) {
    return new AnthropicSdkClient({
      apiKey: config.llm.anthropicApiKey,
      defaultModel: config.llm.model,
    });
  }
  throw new Error("LLM is not configured. Set llm.backend=claude-cli or ANTHROPIC_API_KEY.");
}

function toSlug(value: string): string {
  return value
    .normalize("NFKD")
    .toLowerCase()
    .replace(/[^a-z0-9]+/g, "-")
    .replace(/^-+|-+$/g, "")
    .replace(/-{2,}/g, "-") || "ai-generated-game";
}

function printUsageAndExit(): never {
  console.error(
    "usage:\n" +
      "  crawl.ts import <md-path>\n" +
      "  crawl.ts list\n" +
      "  crawl.ts run <gameName> [out-md-path]\n" +
      "  crawl.ts ext-fetch steam <gameSlug> <appId> [--lang all] [--max N]\n" +
      '  crawl.ts ext-fetch youtube-videos <gameSlug> --q "<query>" [--max N]\n' +
      "  crawl.ts ext-fetch youtube-comments <gameSlug> <videoId> [--max N]\n" +
      "  crawl.ts ext-fetch website <gameSlug> <url> [<url> ...]\n" +
      '  crawl.ts ext-fetch reddit <gameSlug> --q "<query>" [--sub <subreddit>] [--threads N]\n' +
      "  crawl.ts ext-import <jsonl-path>\n" +
      "  crawl.ts ext-ingest steam <gameSlug> <appId> [--lang all] [--max N]\n" +
      "  crawl.ts ext-ingest youtube <gameSlug> <videoId> [--max N]\n" +
      "  crawl.ts ext-ingest website <gameSlug> <url> [<url> ...]\n" +
      '  crawl.ts ext-ingest reddit <gameSlug> --q "<query>" [--sub <subreddit>] [--threads N]'
  );
  process.exit(2);
}

main().catch((err) => {
  console.error((err as Error).message);
  process.exit(1);
});
