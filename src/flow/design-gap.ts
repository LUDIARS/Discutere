/**
 * design_gap 計算 (改善フロー / improvement.md (1))。
 *
 *   現状 (起点) ベクトル = Di 基礎感情データ (`*.sentiment.json` の game_vector)。
 *     起点は negative であることを想定する (改善が必要だから議題になっている)。
 *   目標 ベクトル       = メカニクスの intended_affect (intended_aspects / intended_emotions /
 *                          intended_valence から固定 20 次元へ写像)。
 *   design_gap          = 目標 − 現状 = 目指す改善ベクトル (positive 方向)。
 *
 * ベクトル空間は `sentiment-vector.ts` の固定 20 次元。新しい感情空間は定義しない
 * (t4-improvement.md の out スコープ準拠)。
 */

import fs from "node:fs";
import path from "node:path";
import { ASP_KEYS, EMO_KEYS, DIM, VECTOR_DIMS, subtract } from "./sentiment-vector.js";
import type { MechanicSummary } from "./investigate.js";

const ASP_INDEX = new Map(ASP_KEYS.map((a, i) => [a, 2 + EMO_KEYS.length + i]));
const EMO_INDEX = new Map(EMO_KEYS.map((e, i) => [e, 2 + i]));
const VALENCE_INDEX = VECTOR_DIMS.indexOf("emo.valence");

/**
 * 現状 (起点) が欠落しているときの negative ベースライン。
 * valence を低く (改善が必要な negative 状態)、アスペクト/感情は中立 0.5、meta は控えめ。
 * improvement.md 「起点は negative であることを想定」に従う。
 */
export function negativeBaselineVector(): number[] {
  const v = new Array<number>(DIM).fill(0.5);
  v[VALENCE_INDEX] = 0.2; // negative valence
  v[VECTOR_DIMS.indexOf("meta.positive_ratio")] = 0.2;
  v[VECTOR_DIMS.indexOf("meta.volume_log")] = 0;
  return v;
}

/**
 * テーマに対応する `*.sentiment.json` の game_vector を現状ベクトルとして読む。
 * 見つからなければ null (呼び出し側が negativeBaselineVector へフォールバック)。
 */
export function loadCurrentVector(theme: string, gamesDir = "./data/games"): number[] | null {
  if (!fs.existsSync(gamesDir)) return null;
  const files = fs.readdirSync(gamesDir).filter((f) => f.endsWith(".sentiment.json"));
  const words = theme
    .toLowerCase()
    .split(/\s+/)
    .filter((w) => w.length >= 2);

  for (const f of files) {
    const lower = f.toLowerCase();
    if (!words.some((w) => lower.includes(w))) continue;
    try {
      const data = JSON.parse(fs.readFileSync(path.join(gamesDir, f), "utf-8")) as {
        game_vector?: number[];
      };
      if (Array.isArray(data.game_vector) && data.game_vector.length === DIM) {
        return data.game_vector;
      }
    } catch {
      // パース失敗は無視
    }
  }
  return null;
}

/**
 * メカニクス群から目標感情ベクトルを構築する。
 *   - intended_aspects に挙がったアスペクト次元 → 1 (狙っている)
 *   - intended_emotions に挙がった感情次元     → 1
 *   - intended_valence の集計で emo.valence を決める (positive=1 / negative=0 / 中立=0.5)
 *   - 言及のない次元は 0.5 (中立。design_gap で打ち消され改善方向に寄与しない)
 */
export function buildTargetVector(mechanics: MechanicSummary[]): number[] {
  const target = new Array<number>(DIM).fill(0.5);

  for (const m of mechanics) {
    for (const a of m.intended_aspects ?? []) {
      const idx = ASP_INDEX.get(a);
      if (idx !== undefined) target[idx] = 1;
    }
    for (const e of m.intended_emotions ?? []) {
      const idx = EMO_INDEX.get(e);
      if (idx !== undefined) target[idx] = 1;
    }
  }

  // valence: intended_valence の集計 (positive=+1 / negative=-1) を 0..1 に写像
  const valences = mechanics
    .map((m) => m.intended_valence)
    .filter((v): v is string => typeof v === "string");
  if (valences.length > 0) {
    const sum = valences.reduce(
      (s, v) => s + (v === "positive" ? 1 : v === "negative" ? -1 : 0),
      0
    );
    const mean = sum / valences.length; // -1..1
    target[VALENCE_INDEX] = (mean + 1) / 2; // 0..1
  } else {
    target[VALENCE_INDEX] = 1; // intended は基本 positive 体験を狙う
  }

  return target;
}

/**
 * design_gap = 目標 − 現状。negative 起点 + positive 目標なら positive 方向ベクトルになる。
 */
export function computeDesignGap(current: number[], target: number[]): number[] {
  return subtract(target, current);
}
