type Cluster = { key: string; count: number; sample: string[] };
const cache = new Map<string, { at: number; value: Cluster[] }>();

export function setUncategorizedClustersCache(workspaceId: string, value: Cluster[]) {
  cache.set(workspaceId, { at: Date.now(), value });
}

export function getUncategorizedClustersCache(workspaceId: string): Cluster[] | null {
  const e = cache.get(workspaceId);
  if (!e) return null;
  return e.value;
}
