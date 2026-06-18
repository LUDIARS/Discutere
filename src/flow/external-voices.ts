/**
 * 外部の声検索 (listExternalVoices) の配線ヘルパ。
 *
 * 議論フロー (web / discord) の deps が要求する `(terms, limit) => ContextVoice[]` を、
 * Discatier Core の adapter (`listRelevantExternalVoices`) から組み立てる。
 * これまで WebUI / Discord 経路では未配線で、 RAG・情報密度評価とも材料 0 件で動いていた
 * (docs/sonnet-tasks/README.md T7 follow-up)。 ここを配線して実材料を流す。
 *
 * Core はコール毎に open/close する (buildQueueText と同じ方針)。 議論本体は director が
 * voice-cache で memoize するため呼び出し頻度は低い。
 */

import type { createCore } from "../core/index.js";
import type { ContextVoice } from "./discussion-paper.js";
import { createDiscatierContextProvider } from "../discatier-engine-adapter/index.js";

type Core = ReturnType<typeof createCore>;

/**
 * active KG から外部の声を引く listExternalVoices を作る。
 * @param openCore active KG を開く factory (resolveActiveKgPath 経由)。
 * @param workspaceId 匿名 workspace (個人データ非保管)。
 */
export function makeListExternalVoices(
  openCore: () => Core,
  workspaceId: string
): (terms: string[], limit: number) => ContextVoice[] {
  return (terms: string[], limit: number): ContextVoice[] => {
    const core = openCore();
    try {
      const provider = createDiscatierContextProvider(core);
      const voices = provider.listRelevantExternalVoices?.(workspaceId, terms, limit) ?? [];
      // §6: 露出は出所 (source+URL) のみ透過、 個人アンカーは adapter 側で既にマスク済み。
      return voices.map((v) => ({
        content: v.content,
        source: v.source,
        sourceUrl: v.sourceUrl ?? undefined,
      }));
    } catch {
      return [];
    } finally {
      core.close?.();
    }
  };
}
