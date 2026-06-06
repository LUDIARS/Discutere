/**
 * 学習ビューア (キャッシュ専用・スタンドアロン)。
 *
 *   npx tsx scripts/learning-viewer.ts        # 既定 :3100
 *   DISCUTERE_LEARNING_VIEWER_PORT=4100 ...    # ポート変更
 *
 * `data/learning-cache.sqlite` だけを読み、KG (Core/Kuzu) には一切触れない。よって
 * KG の単一 writer をロックしないため、Bot 稼働中やクロール中でも並行して参照できる。
 * UI (HTML) は本体ダッシュボードの learning-view-routes と共有する。
 * キャッシュ更新は `npm run build:learning-cache`。
 */

import { serve } from "@hono/node-server";
import { Hono } from "hono";

import { HTML } from "../src/api/learning-view-routes.js";
import { normalizeLearningLayer } from "../src/visualize/learning-layers.js";
import {
  DEFAULT_CACHE_PATH,
  learningCacheExists,
  openLearningCacheReader,
} from "../src/visualize/learning-cache.js";

const port = Number(process.env.DISCUTERE_LEARNING_VIEWER_PORT ?? 3100);

if (!learningCacheExists()) {
  console.error(`learning cache が見つかりません: ${DEFAULT_CACHE_PATH}`);
  console.error("先に `npm run build:learning-cache` でキャッシュを生成してください。");
  process.exit(1);
}

const cache = openLearningCacheReader();
const app = new Hono();

app.get("/", (c) => c.redirect("/learning"));
app.get("/learning", (c) => c.html(HTML));
app.get("/learning/conclusions", (c) => c.json(cache.conclusions() as object));
app.get("/learning/conclusion", (c) => {
  const detail = cache.conclusion(c.req.query("gap") ?? "");
  return detail ? c.json(detail as object) : c.json({ error: "not found" }, 404);
});
app.get("/learning/data", (c) => {
  const snapshot = cache.layer(normalizeLearningLayer(c.req.query("layer")));
  return snapshot ? c.json(snapshot as object) : c.json({ error: "layer not cached" }, 404);
});
app.get("/learning/gap", (c) => c.json(cache.gapSummary() as object));
app.get("/learning/gap/detail", (c) => {
  const detail = cache.gap(c.req.query("slug") ?? "");
  return detail ? c.json(detail as object) : c.json({ error: "not found" }, 404);
});

serve({ fetch: app.fetch, port }, (info) => {
  const builtAt = cache.builtAt();
  console.log(`Learning viewer (cache-only / KG 非アクセス) on http://localhost:${info.port}/learning`);
  console.log(builtAt ? `  cache built_at: ${new Date(builtAt).toISOString()}` : "  cache built_at: unknown");
});
