/**
 * 議論を Fundamentum master に put し、catalog "discutere/discussions" へ論理名 (gapId) で登録する。
 * SRP: export の実行 (I/O) のみ。スナップショット整形は snapshot.ts。
 */
import type { ConclusionDetail } from "../visualize/conclusions.js";
import { getFundamentumStore } from "./store.js";
import { buildDiscussionSnapshot } from "./snapshot.js";

export const DISCUSSIONS_CATALOG = "discutere/discussions";

export interface FundamentumExportResult {
  discussionId: string;
  contentId: string;
  catalogVersion: number;
}

export async function exportDiscussionToFundamentum(detail: ConclusionDetail): Promise<FundamentumExportResult> {
  const fm = getFundamentumStore();
  const snapshot = buildDiscussionSnapshot(detail);
  const record = await fm.master.put(snapshot);
  const catalog = await fm.catalog.builder(DISCUSSIONS_CATALOG).set(detail.gapId, record.id).commit();
  return { discussionId: detail.gapId, contentId: record.id, catalogVersion: catalog.version };
}
