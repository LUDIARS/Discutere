/**
 * ゲーム単位の声の引き出し (spec/feature/flow/user-voices.md)。
 *
 * Steam レビューや GLAB の感想は本文にゲーム名を書かないことが多く、議題の語によるキーワード検索
 * (listRelevantExternalVoices) では見つからない。取込時に付けた gameSlug (attribution) でゲーム単位に
 * 引き、議題の埋め込みがあればベクトルの近い順、無ければ新しい順に並べる。
 */

import type { createCore } from "../../core/index.js";
import type { AttributionStore } from "../../crawler/sources/attribution-store.js";
import { fetchVectorsByNodeIds } from "../../core/vectors/vector-search.js";
import { cosineSimilarity } from "../../core/vectors/vector-math.js";
import type { ContextVoice } from "../discussion-paper.js";

type Core = ReturnType<typeof createCore>;

/** 1 ゲームから候補として読む最大件数 (並べ替えの母数)。 */
const CANDIDATE_CAP = 2000;

/** 収集した声の出所 (Steam レビュー / GLAB 感想)。 */
const USER_VOICE_SOURCES = ["steam", "glab"] as const;

/** gameSlug の声を議題に近い順 (ベクトルがあれば) に limit 件返す。 */
export function listGameVoices(args: {
  core: Core;
  attribution: Pick<AttributionStore, "listBySource">;
  workspaceId: string;
  gameSlug: string;
  limit: number;
  /** 議題の埋め込み (無ければ新しい順)。 */
  queryVector?: number[] | null;
}): ContextVoice[] {
  const attributions = USER_VOICE_SOURCES.flatMap((source) =>
    args.attribution.listBySource(source, args.gameSlug, CANDIDATE_CAP)
  );
  if (attributions.length === 0) return [];
  const select = args.core.client.raw.prepare(
    "SELECT raw_content FROM utterances WHERE workspace_id = ? AND id = ?"
  );
  const rows = attributions.flatMap((a) => {
    const row = select.get(args.workspaceId, a.utteranceId) as { raw_content: string | null } | undefined;
    const content = row?.raw_content?.trim();
    return content ? [{ id: a.utteranceId, content, source: a.source, sourceUrl: a.sourceUrl, at: a.importedAt }] : [];
  });

  let ordered = rows;
  if (args.queryVector && args.queryVector.length > 0) {
    const vectors = fetchVectorsByNodeIds(args.core.client, {
      workspaceId: args.workspaceId,
      nodeType: "utterance",
      nodeIds: rows.map((r) => r.id),
    });
    const q = args.queryVector;
    // ベクトルのある声を近い順に先に、未ベクトル化の声はその後ろ (新しい順)。
    const scored = rows.map((r) => {
      const v = vectors.get(r.id);
      return { r, score: v ? cosineSimilarity(q, v) : Number.NEGATIVE_INFINITY };
    });
    scored.sort((a, b) => b.score - a.score || b.r.at - a.r.at);
    ordered = scored.map((s) => s.r);
  }
  return ordered.slice(0, args.limit).map((r) => ({
    content: r.content,
    source: r.source,
    sourceUrl: r.sourceUrl || undefined,
  }));
}
