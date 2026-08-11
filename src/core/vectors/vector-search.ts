import type { KuzuClient } from "../db/kuzu-client.js";

import { cosineSimilarity, isEmbeddingVector } from "./vector-math.js";

/**
 * 候補 node 群の埋め込みベクトルを一括取得する (dense rerank 用)。
 * 全件走査 (searchSimilar) は KG が大きいと重いため、キーワード検索で絞った
 * 候補集合だけをベクトル再ランクする二段構え (recall=キーワード / precision=ベクトル)
 * の下半分として使う。ベクトル未構築の node は結果に含まれない。
 */
export function fetchVectorsByNodeIds(client: KuzuClient, input: {
  workspaceId: string;
  nodeType: string;
  nodeIds: readonly string[];
}): Map<string, number[]> {
  const out = new Map<string, number[]>();
  const CHUNK = 500; // SQLite のバインド変数上限 (999) を割らないよう分割。
  for (let i = 0; i < input.nodeIds.length; i += CHUNK) {
    const chunk = input.nodeIds.slice(i, i + CHUNK);
    const placeholders = chunk.map(() => "?").join(",");
    const rows = client.raw
      .prepare(
        `SELECT node_id, vector_json FROM embeddings
          WHERE workspace_id = ? AND node_type = ? AND node_id IN (${placeholders})`
      )
      .all(input.workspaceId, input.nodeType, ...chunk) as Array<{ node_id: string; vector_json: string }>;
    for (const r of rows) {
      try {
        const vector: unknown = JSON.parse(r.vector_json);
        if (isEmbeddingVector(vector)) out.set(r.node_id, vector);
      } catch {
        // 壊れた行は rerank から外すだけ (再構築は build スクリプトの仕事)。
      }
    }
  }
  return out;
}

export function searchSimilar(client: KuzuClient, input: {
  workspaceId: string;
  vector: number[];
  k: number;
  nodeType?: string;
}) {
  const rows = (input.nodeType
    ? client.raw.prepare("SELECT node_type, node_id, vector_json FROM embeddings WHERE workspace_id = ? AND node_type = ?").all(input.workspaceId, input.nodeType)
    : client.raw.prepare("SELECT node_type, node_id, vector_json FROM embeddings WHERE workspace_id = ?").all(input.workspaceId)
  ) as Array<{ node_type: string; node_id: string; vector_json: string }>;

  return rows
    .flatMap((r) => {
      try {
        const vector: unknown = JSON.parse(r.vector_json);
        return isEmbeddingVector(vector)
          ? [{ nodeType: r.node_type, nodeId: r.node_id, score: cosineSimilarity(input.vector, vector) }]
          : [];
      } catch {
        return [];
      }
    })
    .sort((a, b) => b.score - a.score)
    .slice(0, Math.max(0, input.k));
}
