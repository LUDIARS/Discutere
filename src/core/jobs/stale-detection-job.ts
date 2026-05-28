import type { ReturnTypeCreateCore } from "../projection/types.js";
import { flagStaleHypotheses } from "../hypothesis/stale-detector.js";

let lock = false;
export function runStaleDetectionJob(core: ReturnTypeCreateCore, workspaceId: string, now = Date.now()): string[] {
  if (lock) return [];
  lock = true;
  try {
    return flagStaleHypotheses(core, workspaceId, now);
  } finally {
    lock = false;
  }
}
