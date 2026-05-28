type Rank = { mechanicId: string; name: string; utteranceCount: number; gapCount: number; staleCount: number; score: number };
const cache = new Map<string, { at: number; value: Rank[] }>();

export function setHotspotRankCache(workspaceId: string, value: Rank[]) {
  cache.set(workspaceId, { at: Date.now(), value });
}

export function getHotspotRankCache(workspaceId: string): Rank[] | null {
  const e = cache.get(workspaceId);
  if (!e) return null;
  return e.value;
}
