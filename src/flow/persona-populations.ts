import { cosine } from "./sentiment-vector.js";
import {
  listPoolPersonas,
  type PersonaOrigin,
  type PoolPersona,
} from "./persona-pool.js";

// @spec 母集団レポート (Di → Vo)
export interface PopulationReportEntry {
  pseudoId: string;
  verdict: "major" | "minor";
  ratio: number;
  nearestClusterSize: number;
}

export interface PopulationReport {
  generatedAt: string;
  realPopulation: number;
  entries: PopulationReportEntry[];
}

export interface PopulationReportOptions {
  similarityThreshold?: number;
  majorRatio?: number;
  realOrigins?: PersonaOrigin[];
  generatedAt?: string;
}

interface Cluster {
  centroid: PoolPersona;
  members: PoolPersona[];
}

function importedClusters(imported: PoolPersona[], threshold: number): Cluster[] {
  const clusters: Cluster[] = [];
  for (const persona of [...imported].sort((left, right) => left.id.localeCompare(right.id))) {
    const cluster = clusters.find((item) =>
      cosine(item.centroid.affectVector, persona.affectVector) >= threshold);
    if (cluster) cluster.members.push(persona);
    else clusters.push({ centroid: persona, members: [persona] });
  }
  return clusters;
}

export function estimatePopulationReport(
  options: PopulationReportOptions = {}
): PopulationReport {
  const threshold = options.similarityThreshold ?? 0.9;
  const majorRatio = options.majorRatio ?? 0.15;
  const realOrigins = options.realOrigins ?? ["adopted"];
  // 仮名 user_id が無い imported 行 (旧データ等) は Vo に返す宛先が無いので除外する。
  const imported = listPoolPersonas({ origin: "imported" }).filter((persona) => !!persona.userId);
  const real = realOrigins.flatMap((origin) => listPoolPersonas({ origin }));

  const entries = importedClusters(imported, threshold).flatMap((cluster) => {
    const nearestClusterSize = real.filter((persona) =>
      cosine(cluster.centroid.affectVector, persona.affectVector) >= threshold).length;
    const ratio = real.length === 0
      ? 0
      : Number((nearestClusterSize / real.length).toFixed(4));
    return cluster.members.map((persona) => ({
      pseudoId: persona.userId as string,
      verdict: ratio >= majorRatio ? "major" as const : "minor" as const,
      ratio,
      nearestClusterSize,
    }));
  }).sort((left, right) => left.pseudoId.localeCompare(right.pseudoId));

  return {
    generatedAt: options.generatedAt ?? new Date().toISOString(),
    realPopulation: real.length,
    entries,
  };
}
