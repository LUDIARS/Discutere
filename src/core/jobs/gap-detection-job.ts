import type { ReturnTypeCreateCore } from "../projection/types.js";
import { detectGaps } from "../bridge/gap/detector.js";

let lock = false;

export function runGapDetectionJob(core: ReturnTypeCreateCore, workspaceId: string): string[] {
  if (lock) return [];
  lock = true;
  try {
    return detectGaps(core, workspaceId);
  } finally {
    lock = false;
  }
}
