/**
 * ③ ゲーム全体のメカニクス集合確定 + 内部経済グラフ生成。
 *
 * economy-analyzer (既存) が生成した EconomyGraph を
 * RESTRUCTURE の ResourceEdge 表現に変換して使う。
 * analyzeEconomy が DB キャッシュを持つためここでは再利用する。
 *
 * spec/plan/RESTRUCTURE.md §2-③ / §4-C。
 */

import { join } from "node:path";
import { readFile, writeFile, mkdir } from "node:fs/promises";
import type { GameIntake, EconomyPhase0, Step3Output } from "./types.js";
import type { LlmAdapter } from "./llm-adapter.js";

// ── キャッシュ I/O ────────────────────────────────────────────────────────────

function cacheDir(restructureDir: string, slug: string): string {
  return join(restructureDir, slug);
}

async function loadCached(restructureDir: string, slug: string): Promise<EconomyPhase0 | null> {
  const path = join(cacheDir(restructureDir, slug), "step3.json");
  try {
    const raw = await readFile(path, "utf-8");
    const parsed = JSON.parse(raw) as EconomyPhase0;
    return { ...parsed, cached: true };
  } catch {
    return null;
  }
}

async function saveCache(restructureDir: string, slug: string, output: EconomyPhase0): Promise<void> {
  const dir = cacheDir(restructureDir, slug);
  await mkdir(dir, { recursive: true });
  await writeFile(join(dir, "step3.json"), JSON.stringify(output, null, 2), "utf-8");
}

// ── economy-analyzer 連携 ─────────────────────────────────────────────────────

interface AnalyzerGraphNode {
  id: string;
  type: string;
  label: string;
  description: string;
}

interface AnalyzerGraphEdge {
  source: string;
  target: string;
  type: string;
}

/** economy-analyzer の EconomyGraph から ResourceEdge に変換する。
 *  type が "produces" / "consumes" のエッジのみ採用。
 *  source/target は nodeId → label に解決する。
 */
function convertAnalyzerGraph(
  nodes: AnalyzerGraphNode[],
  edges: AnalyzerGraphEdge[],
): { edges: EconomyPhase0["edges"]; resources: EconomyPhase0["resources"] } {
  const nodeMap = new Map(nodes.map((n) => [n.id, n]));
  const resources: EconomyPhase0["resources"] = [];
  const resourceNames = new Set<string>();
  const resultEdges: EconomyPhase0["edges"] = [];

  for (const edge of edges) {
    if (edge.type !== "produces" && edge.type !== "consumes") continue;
    const src = nodeMap.get(edge.source);
    const tgt = nodeMap.get(edge.target);
    if (!src || !tgt) continue;

    // target が resource / experience タイプならリソースとして登録
    if (tgt.type === "resource" || tgt.type === "experience" || tgt.type === "attrition") {
      if (!resourceNames.has(tgt.label)) {
        resourceNames.add(tgt.label);
        resources.push({ name: tgt.label, description: tgt.description });
      }
      resultEdges.push({
        sourceMechanic: src.label,
        targetResource: tgt.label,
        type: edge.type as "produces" | "consumes",
      });
    }
  }

  return { edges: resultEdges, resources };
}

// ── 公開 API ──────────────────────────────────────────────────────────────────

export async function buildEconomy(
  intake: GameIntake,
  restructureDir: string,
  llm: LlmAdapter,
  opts?: { force?: boolean },
): Promise<Step3Output> {
  // キャッシュ確認
  if (!opts?.force) {
    const cached = await loadCached(restructureDir, intake.slug);
    if (cached) return cached;
  }

  // economy-analyzer が持つ DB キャッシュを試みる
  // (economy-analyzer は workspaceId + gameTitle でキャッシュ管理しているが
  //  RESTRUCTURE パイプラインからは workspaceId にアクセスできないため
  //  LLM アダプタ経由で生成する)
  const economy = await llm.generateEconomyGraph(intake.slug, intake.mechanics);

  const output: Step3Output = { ...economy, cached: false };
  await saveCache(restructureDir, intake.slug, output);
  return output;
}

/** economy-analyzer の既存キャッシュを RESTRUCTURE 形式に変換して利用する。
 *  DB を使える環境向けのオプショナル高速化パス。 */
export async function buildEconomyFromAnalyzerCache(
  intake: GameIntake,
  restructureDir: string,
  analyzeEconomyFn: (workspaceId: string, gameTitle: string, apiKey: string) => Promise<{
    ok: boolean;
    graph?: { nodes: AnalyzerGraphNode[]; edges: AnalyzerGraphEdge[] };
    cached?: boolean;
  }>,
  workspaceId: string,
  apiKey: string,
): Promise<Step3Output | null> {
  const result = await analyzeEconomyFn(workspaceId, intake.title, apiKey);
  if (!result.ok || !result.graph) return null;

  const { edges, resources } = convertAnalyzerGraph(result.graph.nodes, result.graph.edges);

  const output: Step3Output = {
    mechanics: intake.mechanics,
    resources,
    edges,
    cached: result.cached ?? false,
  };
  await mkdir(join(restructureDir, intake.slug), { recursive: true });
  await writeFile(join(restructureDir, intake.slug, "step3.json"), JSON.stringify(output, null, 2), "utf-8");
  return output;
}

/** resource グラフでの次数中心性 (出次 + 入次の合計) を mechanic 名ごとに返す。 */
export function computeEconomyCentrality(economy: EconomyPhase0): Map<string, number> {
  const degree = new Map<string, number>();
  for (const edge of economy.edges) {
    degree.set(edge.sourceMechanic, (degree.get(edge.sourceMechanic) ?? 0) + 1);
  }
  const max = Math.max(1, ...degree.values());
  const normalized = new Map<string, number>();
  for (const [name, d] of degree) {
    normalized.set(name, d / max);
  }
  return normalized;
}
