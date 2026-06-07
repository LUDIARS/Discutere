/**
 * Learning View キャッシュ生成 CLI。
 *
 *   npx tsx scripts/build-learning-cache.ts
 *
 * KG (Core) から学習ビューアの全クエリを 1 回実行し、`data/learning-cache.sqlite` に固める。
 * データ追加・議論収束のあとに実行する。DB path は env DISCATIER_KUZU_PATH > 既定。
 */

import path from "node:path";

import { getConfig } from "../src/config.js";
import { createCore } from "../src/core/index.js";
import { buildLearningCache, buildGapCache } from "../src/visualize/learning-cache.js";

function main(): void {
  const core = createCore();
  let result;
  try {
    result = buildLearningCache(core, getConfig().workspace);
  } finally {
    core.close();
  }
  // Gap率 (運営想定 vs 観測) を data/games/*.md から算出して同 cache に焼く (KG は read-only)。
  const gap = buildGapCache(result.path);
  console.log(
    JSON.stringify(
      {
        path: path.relative(process.cwd(), result.path),
        builtAt: new Date(result.builtAt).toISOString(),
        conclusions: result.conclusions,
        details: result.details,
        layers: result.layers,
        sources: result.sources,
        gapGames: gap.games,
      },
      null,
      2
    )
  );
}

main();
