/**
 * 母数推定 CLI (C2-b, 案A)。
 *
 *   npm run persona:populations -- [--threshold 0.9] [--big-ratio 0.15] [--real adopted,seed] [--no-persist]
 *
 * 合成ペルソナ (C2) をクラスタリングし、各クラスタが実クロール分布 (C1 adopted) で
 * どれだけ近傍を持つかで母数の大小を推定して表示する。
 * 既定で推定値 (大/小・比率) を flow_persona に書き戻す (C2-b 永続化、--no-persist で抑止)。
 */

import { estimatePopulations, persistPopulations } from "../src/flow/persona-populations.js";
import type { PersonaOrigin } from "../src/flow/persona-pool.js";

function arg(name: string): string | undefined {
  const i = process.argv.indexOf(`--${name}`);
  return i >= 0 ? process.argv[i + 1] : undefined;
}
function has(name: string): boolean {
  return process.argv.includes(`--${name}`);
}

async function main(): Promise<void> {
  const simThreshold = arg("threshold") ? Number(arg("threshold")) : undefined;
  const bigRatio = arg("big-ratio") ? Number(arg("big-ratio")) : undefined;
  const realOrigins = arg("real") ? (arg("real")!.split(",").map((s) => s.trim()) as PersonaOrigin[]) : undefined;
  const persist = !has("no-persist");

  const report = estimatePopulations({ simThreshold, bigRatio, realOrigins });
  console.log(`実分布 ${report.realCount} / 合成 ${report.syntheticCount} / クラスタ ${report.clusters.length}`);
  console.log("─".repeat(60));
  for (const c of report.clusters.sort((a, b) => b.realRatio - a.realRatio)) {
    console.log(
      `[${c.verdict}] cluster size=${c.size} 実近傍=${c.realNeighbors} (ratio=${c.realRatio}) centroid=${c.centroidPersonaId}`
    );
  }
  if (report.realCount === 0) {
    console.log("\n⚠️ 実分布 (adopted) が空です。先に npm run persona:adopt で実ユーザを採用してください。");
  }
  if (persist && report.syntheticCount > 0) {
    const written = persistPopulations(report, Date.now());
    console.log(`\n💾 母数推定値を ${written} 体の合成ペルソナへ保存しました (--no-persist で抑止)。`);
  } else if (!persist) {
    console.log("\n(--no-persist: DB へは保存しません)");
  }
}

main().catch((e) => {
  console.error(`NG: ${(e as Error).message}`);
  process.exit(1);
});
