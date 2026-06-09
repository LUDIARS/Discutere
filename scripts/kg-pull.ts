/**
 * 共有 KG を配信元から今すぐ pull する (npm run kg:pull [URL] [--full]).
 *
 * URL 省略時は config.kgSync.sourceUrl。--full で透かしを無視して全件取り直す。
 * follower (master ではない Di パッケージ) で使う。
 */

import { getConfig } from "../src/config.js";
import { createCore } from "../src/core/index.js";
import { resolveActiveKgPath } from "../src/core/kg-registry.js";
import { pullSharedKg } from "../src/kg-sync/pull.js";

const config = getConfig();
const args = process.argv.slice(2);
const url = args.find((a) => !a.startsWith("--")) ?? config.kgSync.sourceUrl;
const full = args.includes("--full");

if (!url) {
  console.error("URL がありません。引数で渡すか config.kgSync.sourceUrl / DISCUTERE_KG_SYNC_URL を設定してください。");
  process.exit(1);
}

const core = createCore(resolveActiveKgPath(config));
try {
  const r = await pullSharedKg(core.client.raw, {
    url,
    statePath: config.kgSync.stateFile,
    token: config.kgSync.token,
    full,
  });
  console.log(
    `✅ KG pull: ${r.url}\n   applied=${r.applied} skipped=${r.skipped} ignored=${r.ignored}` +
      ` watermark ${r.fromWatermark} -> ${r.toWatermark}`
  );
} catch (err) {
  console.error("❌ KG pull failed:", (err as Error).message);
  process.exit(1);
} finally {
  core.close();
}
