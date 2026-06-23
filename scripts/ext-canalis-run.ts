/**
 * 外部データ Canalis パイプライン実行スクリプト。
 *
 * @ludiars/canalis の YouTubeSource / RedditSource で ① クロール →
 * Di の ExternalSentimentTransform で ② 感情カスケード →
 * Di の KuzuGraphClient で ③ KgBatch 書込。
 *
 * 使い方:
 *   npm run ext:canalis -- --source youtube --video-id <videoId> --slug hollow-knight
 *   npm run ext:canalis -- --source reddit  --subreddit HollowKnight --slug hollow-knight
 *   npm run ext:canalis -- --dry-run   # クロールのみ (Kuzu 書込なし)
 *
 * 必要な暗号化 config:
 *   DISCUTERE_YOUTUBE_API_KEY    (youtube のみ)
 *
 * 任意の環境変数:
 *   LLM_LOCAL_BASE_URL           (Tier 1 ローカル LLM。省略時は Tier 0 + Residual のみ)
 *   ANTHROPIC_API_KEY            (Residual LLM。省略時は Tier 0/1 で打ち止め)
 */

import { YouTubeSource, RedditSource } from "@ludiars/canalis";
import type { RawRecord, KgBatch } from "@ludiars/canalis";
import { ExternalSentimentTransform } from "../src/crawler/canalis-bridge.js";
import { openKuzuGraph } from "../src/core/db/kuzu-graph-client.js";
import { LocalOpenAiClient } from "../src/persona-engine/llm/local-openai.js";
import { AnthropicSdkClient } from "../src/persona-engine/llm/anthropic.js";
import type { LLMClient } from "../src/persona-engine/llm/client.js";
import { resolveActiveKgPath } from "../src/core/kg-registry.js";
import { loadConfig } from "../src/config.js";
import { getInfisicalRuntimeSecret } from "../src/secrets/infisical-runtime.js";

function parseArgs(argv: string[]): {
  source: string;
  slug: string;
  videoId?: string;
  subreddit?: string;
  maxResults?: number;
  sort?: "hot" | "new" | "top" | "rising";
  dryRun: boolean;
} {
  const get = (name: string) => { const i = argv.indexOf(`--${name}`); return i >= 0 ? argv[i + 1] : undefined; };
  const flag = (name: string) => argv.includes(`--${name}`);
  const sort = get("sort") as "hot" | "new" | "top" | "rising" | undefined;
  return {
    source: get("source") ?? "youtube",
    slug: get("slug") ?? "unknown",
    videoId: get("video-id"),
    subreddit: get("subreddit"),
    maxResults: get("max-results") ? Number(get("max-results")) : undefined,
    sort: ["hot", "new", "top", "rising"].includes(sort ?? "") ? sort : undefined,
    dryRun: flag("dry-run"),
  };
}

async function crawl(args: ReturnType<typeof parseArgs>): Promise<RawRecord[]> {
  if (args.source === "youtube") {
    const apiKey = await getInfisicalRuntimeSecret("DISCUTERE_YOUTUBE_API_KEY");
    if (!apiKey) throw new Error("DISCUTERE_YOUTUBE_API_KEY is not set in encrypted config");
    if (!args.videoId) throw new Error("--video-id is required for source=youtube");
    const src = new YouTubeSource();
    return src.crawl({ videoId: args.videoId, apiKey, maxResults: args.maxResults ?? 200 });
  }
  if (args.source === "reddit") {
    if (!args.subreddit) throw new Error("--subreddit is required for source=reddit");
    const src = new RedditSource();
    return src.crawl({
      subreddit: args.subreddit,
      sort: args.sort ?? "top",
      limit: args.maxResults ?? 100,
      mode: "posts",
    });
  }
  throw new Error(`Unknown source: ${args.source}. Use: youtube | reddit`);
}

function buildClients(): { local?: LLMClient; main?: LLMClient } {
  const localBase = process.env.LLM_LOCAL_BASE_URL;
  const anthropicKey = process.env.ANTHROPIC_API_KEY;
  return {
    local: localBase ? new LocalOpenAiClient({ baseUrl: localBase, defaultModel: "gemma4:12b" }) : undefined,
    main: anthropicKey ? new AnthropicSdkClient({ apiKey: anthropicKey }) : undefined,
  };
}

async function writeToKuzu(batches: KgBatch[], kgDir: string): Promise<void> {
  const kuzu = await openKuzuGraph(kgDir);
  try {
    for (const batch of batches) {
      for (const node of batch.nodes) {
        // MERGE でノードを upsert (id で同定)
        const propEntries = Object.entries(node.props ?? {});
        const propClause = propEntries.map(([k]) => `n.${k} = $${k}`).join(", ");
        const params: Record<string, unknown> = { id: node.key };
        for (const [k, v] of propEntries) params[k] = v;
        const cypher = propClause
          ? `MERGE (n:${node.label} {id: $id}) ON CREATE SET ${propClause} ON MATCH SET ${propClause}`
          : `MERGE (n:${node.label} {id: $id})`;
        await kuzu.run(cypher, params as never);
      }
      for (const edge of batch.edges) {
        await kuzu.run(
          `MATCH (a:${edge.from.label} {id: $fromKey}), (b:${edge.to.label} {id: $toKey}) MERGE (a)-[:${edge.label}]->(b)`,
          { fromKey: edge.from.key, toKey: edge.to.key } as never,
        );
      }
    }
  } finally {
    kuzu.close();
  }
}

async function main(): Promise<void> {
  const args = parseArgs(process.argv.slice(2));
  console.log(`[canalis] source=${args.source} slug=${args.slug} dry-run=${args.dryRun}`);

  const records = await crawl(args);
  console.log(`[canalis] crawled ${records.length} records`);

  const transform = new ExternalSentimentTransform({
    gameSlug: args.slug,
    clients: buildClients(),
    onError: (err, rec) => console.error(`[cascade] ${rec.sourceId}: ${err.message}`),
  });
  const batches = await transform.run(records);
  const totalNodes = batches.reduce((s, b) => s + b.nodes.length, 0);
  console.log(`[canalis] sentiment batches=${batches.length} nodes=${totalNodes}`);

  if (args.dryRun) {
    console.log("[canalis] dry-run — Kuzu 書込をスキップ");
    return;
  }

  const config = loadConfig();
  const kgDir = resolveActiveKgPath(config) ?? "./data/discatier.kuzu";
  await writeToKuzu(batches, kgDir);
  console.log(`[canalis] Kuzu 書込完了 → ${kgDir}`);
}

main().catch((err: unknown) => {
  console.error("[canalis] fatal:", (err as Error).message);
  process.exitCode = 1;
});
