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
import { buildLearningCache } from "../src/visualize/learning-cache.js";

function main(): void {
  const core = createCore();
  try {
    const result = buildLearningCache(core, getConfig().workspace);
    console.log(
      JSON.stringify(
        {
          path: path.relative(process.cwd(), result.path),
          builtAt: new Date(result.builtAt).toISOString(),
          conclusions: result.conclusions,
          details: result.details,
          layers: result.layers,
        },
        null,
        2
      )
    );
  } finally {
    core.close();
  }
}

main();
