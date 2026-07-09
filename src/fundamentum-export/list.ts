/**
 * export 済み議論の一覧・個別取得・バージョン履歴。
 * SRP: catalog/master からの読み出しのみ (export の実行は export.ts)。
 */
import type { JsonValue } from "fundamentum";
import { getFundamentumStore } from "./store.js";
import { DISCUSSIONS_CATALOG } from "./export.js";

export interface ExportedDiscussionRef {
  discussionId: string;
  contentId: string;
}

/** catalog の最新版に載っている全議論 (discussionId 順)。 */
export async function listExportedDiscussions(): Promise<ExportedDiscussionRef[]> {
  const fm = getFundamentumStore();
  const catalog = await fm.catalog.latest(DISCUSSIONS_CATALOG);
  if (!catalog) return [];
  return Object.entries(catalog.entries)
    .map(([discussionId, contentId]) => ({ discussionId, contentId }))
    .sort((a, b) => a.discussionId.localeCompare(b.discussionId));
}

/** 1 議論の最新スナップショット (export 未実施なら null)。 */
export async function getExportedDiscussionSnapshot(discussionId: string): Promise<JsonValue | null> {
  const refs = await listExportedDiscussions();
  const ref = refs.find((r) => r.discussionId === discussionId);
  if (!ref) return null;
  const fm = getFundamentumStore();
  return fm.master.get(ref.contentId);
}

export interface DiscussionRevision {
  /** catalog 全体の版番号 (他議論の export でも進む点に注意)。 */
  version: number;
  contentId: string;
}

/**
 * 1 議論の content id がバージョンを跨いでどう変わったか (再 export で内容が変わった版だけ)。
 * catalog.history() は最新が先頭なので古い→新しい順に辿って変化点だけ拾う。
 */
export async function getDiscussionExportHistory(discussionId: string): Promise<DiscussionRevision[]> {
  const fm = getFundamentumStore();
  const versions = await fm.catalog.history(DISCUSSIONS_CATALOG);
  const out: DiscussionRevision[] = [];
  let lastContentId: string | undefined;
  for (const catalog of [...versions].reverse()) {
    const contentId = catalog.entries[discussionId];
    if (contentId && contentId !== lastContentId) {
      out.push({ version: catalog.version, contentId });
      lastContentId = contentId;
    }
  }
  return out;
}
