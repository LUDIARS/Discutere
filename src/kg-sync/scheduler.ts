/**
 * follower 側の自動 pull スケジューラ — spec/data/core/KG-SYNC.md。
 *
 * config.kgSync.auto かつ sourceUrl がある場合のみ起動。起動直後に 1 回 + intervalHours ごとに
 * 共有 KG を pull する。master では通常 auto=false (配信側なので pull しない)。
 */

import type { DiscutereConfig } from "../config.js";
import { createCore } from "../core/index.js";
import { resolveActiveKgPath } from "../core/kg-registry.js";
import { pullSharedKg } from "./pull.js";

export function startKgSyncScheduler(config: DiscutereConfig): () => void {
  const { auto, sourceUrl, intervalHours, stateFile, token } = config.kgSync;
  if (!auto || !sourceUrl) {
    console.log("  kg-sync: scheduler skipped (auto=false または sourceUrl 未設定)");
    return () => {};
  }

  const runOnce = async (): Promise<void> => {
    const core = createCore(resolveActiveKgPath(config));
    try {
      const r = await pullSharedKg(core.client.raw, { url: sourceUrl, statePath: stateFile, token });
      console.log(
        `  kg-sync: pulled applied=${r.applied} skipped=${r.skipped} ignored=${r.ignored} watermark=${r.toWatermark}`
      );
    } catch (err) {
      console.warn(`  kg-sync: pull failed: ${(err as Error).message}`);
    } finally {
      core.close();
    }
  };

  void runOnce();
  const ms = Math.max(1, intervalHours) * 60 * 60 * 1000;
  const timer = setInterval(() => void runOnce(), ms);
  timer.unref?.();
  console.log(`  kg-sync: auto pull every ${intervalHours}h from ${sourceUrl}`);
  return () => clearInterval(timer);
}
