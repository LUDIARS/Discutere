import type { ReturnTypeCreateCore } from "../../types.js";
import { getHotspotRankCache, setHotspotRankCache } from "../../../cache/hotspot-rank.js";

export function runHotspotAggregationJob(core: ReturnTypeCreateCore, workspaceId: string) {
  const mechanics = core.client.raw.prepare("SELECT id,name FROM mechanics WHERE workspace_id = ?").all(workspaceId) as any[];
  const ranks = mechanics.map((m) => {
    const utteranceCount = (core.client.raw.prepare("SELECT COUNT(*) c FROM utterances WHERE workspace_id = ? AND raw_content LIKE ?").get(workspaceId, `%${m.name}%`) as any).c as number;
    const gapCount = (core.client.raw.prepare("SELECT COUNT(*) c FROM design_gaps WHERE workspace_id = ? AND gap_in = ?").get(workspaceId, m.id) as any).c as number;
    const staleCount = (core.client.raw.prepare("SELECT COUNT(*) c FROM hypotheses WHERE workspace_id = ? AND stale_flagged = 1 AND statement LIKE ?").get(workspaceId, `%${m.name}%`) as any).c as number;
    const score = utteranceCount * 0.5 + gapCount * 2 + staleCount * 1.5;
    return { mechanicId: m.id, name: m.name, utteranceCount, gapCount, staleCount, score };
  }).sort((a, b) => b.score - a.score);
  setHotspotRankCache(workspaceId, ranks);
  return ranks;
}

export function handleHotspot(core: ReturnTypeCreateCore, workspaceId: string): string {
  const ranks = getHotspotRankCache(workspaceId) ?? runHotspotAggregationJob(core, workspaceId);
  return ranks.slice(0, 10).map((r, i) => `${i + 1}. ${r.name} score=${r.score.toFixed(2)} (u:${r.utteranceCount} g:${r.gapCount} s:${r.staleCount})`).join("\n") || "no hotspots";
}
