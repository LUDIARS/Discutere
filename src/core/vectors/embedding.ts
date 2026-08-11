import { randomUUID } from "node:crypto";

import type { KuzuClient } from "../db/kuzu-client.js";
import { isEmbeddingVector } from "./vector-math.js";

export function registerEmbedding(client: KuzuClient, input: {
  workspaceId: string;
  nodeType: string;
  nodeId: string;
  vector: number[];
}): void {
  if (!isEmbeddingVector(input.vector)) {
    throw new Error("embedding vector must be a non-empty array of finite numbers");
  }
  client.raw.prepare(
    `INSERT INTO embeddings (id, workspace_id, node_type, node_id, vector_json, created_at)
     VALUES (?, ?, ?, ?, ?, ?)
     ON CONFLICT(workspace_id, node_type, node_id) DO UPDATE SET vector_json = excluded.vector_json, created_at = excluded.created_at`
  ).run(randomUUID(), input.workspaceId, input.nodeType, input.nodeId, JSON.stringify(input.vector), Date.now());
}
