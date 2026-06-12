/**
 * Kuzu グラフを events から再構築する — spec/core/KUZU-MIGRATION.md §6/§8-3。
 *
 * 使い方:
 *   npx tsx scripts/rebuild-kuzu.ts              # events → Kuzu に全 replay
 *   npx tsx scripts/rebuild-kuzu.ts --verify     # replay 後 SQLite と件数照合
 *   npx tsx scripts/rebuild-kuzu.ts --counts     # 現状のノード件数だけ表示
 *
 * Kuzu DB パスは discutere.config.json の discatier.kuzuGraphPath。
 * 未設定なら ./data/discatier-graph.kuzu を使う。
 */

import { getConfig } from "../src/config.js";
import { resolveActiveKgPath } from "../src/core/kg-registry.js";
import { createCore } from "../src/core/index.js";
import { openKuzuGraph } from "../src/core/db/kuzu-graph-client.js";

async function main(): Promise<void> {
  const args = process.argv.slice(2);
  const verify = args.includes("--verify");
  const countsOnly = args.includes("--counts");

  const config = getConfig();
  const sqlitePath = resolveActiveKgPath(config);

  // Kuzu DB パスは config 優先、未設定は sqlite 隣
  const kuzuPath = (config.discatier as Record<string, unknown>).kuzuGraphPath as string | undefined
    ?? "./data/discatier-graph.kuzu";

  console.log(`SQLite: ${sqlitePath ?? "(default)"}`);
  console.log(`Kuzu:   ${kuzuPath}`);

  const graph = await openKuzuGraph(kuzuPath);

  if (countsOnly) {
    const counts = await graph.nodeCountByType();
    console.log("\nKuzu ノード件数:");
    for (const [t, n] of Object.entries(counts)) {
      console.log(`  ${t}: ${n}`);
    }
    graph.close();
    return;
  }

  const core = createCore({ sqlitePath, graph });

  try {
    const { applied, failed } = await core.eventLog.rebuildKuzu();
    console.log(`\nKuzu rebuild 完了: applied=${applied} failed=${failed}`);

    if (verify) {
      await verifyConsistency(core, graph);
    }
  } finally {
    core.close(); // graph.close() も内包
  }
}

async function verifyConsistency(
  core: ReturnType<typeof createCore>,
  graph: Awaited<ReturnType<typeof openKuzuGraph>>,
): Promise<void> {
  const db = core.client.raw;

  const kuzuCounts = await graph.nodeCountByType();

  const sqliteTables: Record<string, string> = {
    Person: "people",
    Game: "games",
    Mechanic: "mechanics",
    Aesthetic: "aesthetics",
    Affect: "affects",
    PlayContext: "play_contexts",
    Session: "sessions",
    Utterance: "utterances",
    Reaction: "reactions",
    DesignGap: "design_gaps",
    Hypothesis: "hypotheses",
  };

  console.log("\n--- 件数照合 (Kuzu vs SQLite) ---");
  let allMatch = true;
  for (const [nodeType, table] of Object.entries(sqliteTables)) {
    const sqliteCount = (db.prepare(`SELECT COUNT(*) as cnt FROM ${table}`).get() as { cnt: number }).cnt;
    const kuzuCount = kuzuCounts[nodeType] ?? -1;
    const match = sqliteCount === kuzuCount;
    if (!match) allMatch = false;
    const mark = match ? "✓" : "✗";
    console.log(`  ${mark} ${nodeType}: SQLite=${sqliteCount} Kuzu=${kuzuCount}`);
  }
  console.log(allMatch ? "\n差分ゼロ — 整合性 OK" : "\n差分あり — Kuzu rebuild を再試行するか events を確認してください");
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
