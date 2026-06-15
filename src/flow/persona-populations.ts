/**
 * 母数推定 (C2-b, 案A)。
 *
 * 合成ペルソナ (origin="generated", C2) の意見をクラスタリングし、各クラスタが
 * **実クロール由来 (origin="adopted", C1) の分布**でどれだけ近傍を持つかで「現実の母数が
 * 大きい/小さい」を推定する。合成数そのものは母数でない (案A = 実データ突合)。
 * 身内データ (実議論 persona 等) も実分布の 1 サンプルとして混ぜられる (realOrigins で指定)。
 *
 * クラスタリングは affect 20 次元の cosine による単純な貪欲法。
 */

import { cosine } from "./sentiment-vector.js";
import { listPoolPersonas, type PoolPersona, type PersonaOrigin } from "./persona-pool.js";

export interface PopulationOptions {
  /** 同一クラスタ / 近傍とみなす cosine 閾値 (既定 0.9)。 */
  simThreshold?: number;
  /** 母数「大」と判定する実近傍比率 (実分布に対する割合, 既定 0.15)。 */
  bigRatio?: number;
  /** 実分布とみなす origin (既定 ["adopted"])。身内 sample を足すなら ["adopted","seed"] 等。 */
  realOrigins?: PersonaOrigin[];
}

export interface PopulationCluster {
  centroidPersonaId: string;
  members: string[]; // 合成ペルソナ id
  size: number; // クラスタ内合成個体数
  realNeighbors: number; // 実分布で近傍にある個体数
  realRatio: number; // realNeighbors / 実分布総数
  verdict: "大" | "小";
}

export interface PopulationReport {
  realCount: number;
  syntheticCount: number;
  clusters: PopulationCluster[];
}

/** centroid に対する近傍数を実分布から数える。 */
function countNeighbors(centroid: number[], pool: PoolPersona[], threshold: number): number {
  return pool.filter((p) => cosine(centroid, p.affectVector) >= threshold).length;
}

/**
 * 合成ペルソナをクラスタリングし、実分布突合で母数を推定する。
 */
export function estimatePopulations(opts: PopulationOptions = {}): PopulationReport {
  const threshold = opts.simThreshold ?? 0.9;
  const bigRatio = opts.bigRatio ?? 0.15;
  const realOrigins = opts.realOrigins ?? ["adopted"];

  const synthetic = listPoolPersonas({ origin: "generated" });
  const real = realOrigins.flatMap((o) => listPoolPersonas({ origin: o }));

  // 合成個体を貪欲クラスタリング (先頭一致するクラスタへ、無ければ新規)。
  const clusters: Array<{ centroid: PoolPersona; members: PoolPersona[] }> = [];
  for (const s of synthetic) {
    const hit = clusters.find((c) => cosine(c.centroid.affectVector, s.affectVector) >= threshold);
    if (hit) hit.members.push(s);
    else clusters.push({ centroid: s, members: [s] });
  }

  const realCount = real.length;
  const report: PopulationReport = {
    realCount,
    syntheticCount: synthetic.length,
    clusters: clusters.map((c) => {
      const realNeighbors = countNeighbors(c.centroid.affectVector, real, threshold);
      const realRatio = realCount > 0 ? +(realNeighbors / realCount).toFixed(4) : 0;
      return {
        centroidPersonaId: c.centroid.id,
        members: c.members.map((m) => m.id),
        size: c.members.length,
        realNeighbors,
        realRatio,
        verdict: realRatio >= bigRatio ? "大" : "小",
      };
    }),
  };
  return report;
}
