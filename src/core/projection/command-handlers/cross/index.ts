import type { HandlerContext, HandlerResult } from "../types.js";
import { handleMeRole } from "./me-role.js";
import { handleFind } from "./find.js";
import { handleLineage } from "./lineage.js";
import { handleCluster as clusterText } from "./cluster.js";
import { handleStale } from "./stale.js";
import { handleHotspot as hotspotText } from "./hotspot.js";

export function handleCross(ctx: HandlerContext, command: string): HandlerResult {
  if (command === "me") {
    if ((ctx.args[0] ?? "").toLowerCase() !== "role") return { ok: false, error: "usage: /me role <...>" };
    const next = { ...ctx, args: ctx.args.slice(1) };
    return handleMeRole(next);
  }
  if (command === "find") return handleFind(ctx);
  if (command === "lineage") return handleLineage(ctx);
  if (command === "cluster") return { ok: true, message: clusterText(ctx.core, ctx.workspaceId) };
  if (command === "stale") return handleStale(ctx);
  if (command === "hotspot") return { ok: true, message: hotspotText(ctx.core, ctx.workspaceId) };
  return { ok: false, error: `unknown cross command: ${command}` };
}
