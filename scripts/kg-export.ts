/**
 * 共有 KG の差分を NDJSON にエクスポートする (npm run kg:export -- [--since N] [--out file]).
 *
 * master 側で静的 publish 用ファイルを作るのに使う (S3/Pages 等へ置けば follower が GET 可能)。
 * --out 省略時は stdout。--since 省略時は全件 (since=0)。
 */

import { writeFileSync } from "node:fs";

import { getConfig } from "../src/config.js";
import { createCore } from "../src/core/index.js";
import { resolveActiveKgPath } from "../src/core/kg-registry.js";
import { exportSharedKg } from "../src/kg-sync/export.js";

const config = getConfig();
const args = process.argv.slice(2);
const sinceIdx = args.indexOf("--since");
const since = sinceIdx >= 0 ? Number(args[sinceIdx + 1]) : 0;
const outIdx = args.indexOf("--out");
const out = outIdx >= 0 ? args[outIdx + 1] : undefined;

if (!Number.isFinite(since) || since < 0) {
  console.error("--since は 0 以上の数値で指定してください。");
  process.exit(1);
}

const core = createCore(resolveActiveKgPath(config));
try {
  const r = exportSharedKg(core.client.raw, { since, workspaceId: config.workspace });
  if (out) {
    writeFileSync(out, r.ndjson, "utf8");
    console.error(`✅ exported ${r.count} rows -> ${out} (watermark=${r.watermark})`);
  } else {
    process.stdout.write(r.ndjson);
    console.error(`✅ exported ${r.count} rows (watermark=${r.watermark})`);
  }
} finally {
  core.close();
}
