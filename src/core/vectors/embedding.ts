import { randomUUID } from "node:crypto";

import type { KuzuClient } from "../db/kuzu-client.js";

export function registerEmbedding(client: KuzuClient, input: {
  workspaceId: string;
  nodeType: string;
  nodeId: string;
  vector: number[];
}): void {
  client.raw.prepare(
    `INSERT INTO embeddings (id, workspace_id, node_type, node_id, vector_json, created_at)
     VALUES (?, ?, ?, ?, ?, ?)
     ON CONFLICT(workspace_id, node_type, node_id) DO UPDATE SET vector_json = excluded.vector_json, created_at = excluded.created_at`
  ).run(randomUUID(), input.workspaceId, input.nodeType, input.nodeId, JSON.stringify(input.vector), Date.now());
}
