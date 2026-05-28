import type { ReturnTypeCreateCore } from "../../types.js";
import { getUncategorizedClustersCache, setUncategorizedClustersCache } from "../../../cache/uncategorized-clusters.js";

export function runUncategorizedClusteringJob(core: ReturnTypeCreateCore, workspaceId: string) {
  const rows = core.client.raw.prepare("SELECT id,raw_content FROM utterances WHERE workspace_id = ? AND normalized_content LIKE '%[uncategorized]%' ORDER BY created_at DESC").all(workspaceId) as any[];
  const groups = new Map<string, string[]>();
  for (const r of rows) {
    const key = (r.raw_content.split(/\s+/)[0] || "misc").toLowerCase();
    const arr = groups.get(key) ?? [];
    arr.push(r.id);
    groups.set(key, arr);
  }
  const clusters = Array.from(groups.entries()).map(([k, ids]) => ({ key: k, count: ids.length, sample: ids.slice(0, 3) }));
  setUncategorizedClustersCache(workspaceId, clusters);
  return clusters;
}

export function handleCluster(core: ReturnTypeCreateCore, workspaceId: string): string {
  const cached = getUncategorizedClustersCache(workspaceId);
  const clusters = cached ?? runUncategorizedClusteringJob(core, workspaceId);
  return clusters.length ? clusters.map((c) => `- ${c.key}: ${c.count}`).join("\n") : "no clusters";
}
