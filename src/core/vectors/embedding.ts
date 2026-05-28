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

export async function generateEmbedding(text: string): Promise<number[]> {
  const apiKey = process.env.OPENAI_API_KEY;
  if (!apiKey) return fallbackEmbedding(text);
  const res = await fetch("https://api.openai.com/v1/embeddings", {
    method: "POST",
    headers: {
      "content-type": "application/json",
      authorization: `Bearer ${apiKey}`,
    },
    body: JSON.stringify({
      model: process.env.EMBEDDING_MODEL ?? "text-embedding-3-small",
      input: text,
    }),
  });
  if (!res.ok) return fallbackEmbedding(text);
  const body: any = await res.json();
  const vec = body?.data?.[0]?.embedding;
  return Array.isArray(vec) ? vec.map((v: any) => Number(v) || 0) : fallbackEmbedding(text);
}

function fallbackEmbedding(text: string): number[] {
  const out = new Array<number>(64).fill(0);
  for (let i = 0; i < text.length; i++) out[i % out.length] += text.charCodeAt(i) / 65535;
  return out;
}
