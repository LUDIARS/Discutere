import type { KuzuClient } from "../db/kuzu-client.js";

function cosineSimilarity(a: number[], b: number[]): number {
  let dot = 0;
  let na = 0;
  let nb = 0;
  const len = Math.min(a.length, b.length);
  for (let i = 0; i < len; i += 1) {
    dot += a[i] * b[i];
    na += a[i] * a[i];
    nb += b[i] * b[i];
  }
  if (na <= 0 || nb <= 0) return 0;
  return dot / (Math.sqrt(na) * Math.sqrt(nb));
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
    .map((r) => ({ nodeType: r.node_type, nodeId: r.node_id, score: cosineSimilarity(input.vector, JSON.parse(r.vector_json)) }))
    .sort((a, b) => b.score - a.score)
    .slice(0, Math.max(0, input.k));
}
