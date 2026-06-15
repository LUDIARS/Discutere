/**
 * 実在ユーザ採用 (C1)。
 *
 * クロール済みの外部発話を話者アンカー (`ext:<source>:<authorId>`) で集約し、
 * 「意思ある人 (ゲーム嗜好あり)」をペルソナとしてプールに採用する。
 *
 * 採用条件 (全て満たす, spec/flow/persona-pool.md §C1):
 *  - 意見が全ゲーム横断で minOpinions (既定 10) 以上。
 *  - 全ネガ / 全ポジを除外 (polarity が片寄りきっている人は弾く)。
 *  - ゲーム間で affect の差分 (gap) がある (≥2 ゲーム + per-game 平均ベクトルが同一でない)。
 *
 * affect は本人の偏りを保持 (= 本人意見の (重み付き) 平均)。母集団平均からの近さ (typicality) を
 * 併せて記録する。口調は付けない (LLM 不要 = 安価)。露出名は `論者#xxxxxx` (個人データ方針)。
 * 保存は source_speaker_id で upsert (再クロールで別個体を量産しない)。
 */

import { randomUUID } from "node:crypto";
import { textToVector, cosine, DIM } from "./sentiment-vector.js";
import { maskedPersonaLabel } from "../crawler/sources/persona.js";
import {
  upsertPoolPersonaBySpeaker,
  listPoolPersonas,
  setPoolPersonaTypicality,
  type PoolPersona,
} from "./persona-pool.js";

/** 1 話者の意見群 (KG 集約結果)。 */
export interface SpeakerOpinions {
  /** `ext:<source>:<authorId>` */
  speakerId: string;
  source?: string;
  opinions: Array<{ text: string; gameSlug: string | null; weight?: number }>;
}

export interface AdoptOptions {
  /** 採用に必要な最小意見数 (既定 10)。 */
  minOpinions?: number;
  /** valence 中立幅 (0..1, 既定 0.05)。pos: v>0.5+eps / neg: v<0.5-eps。 */
  polarityEps?: number;
  /** ゲーム間 gap の最小 (per-game 平均ベクトルの最大対ユークリッド距離, 既定 0.05)。 */
  gameGapEps?: number;
}

export type RejectReason = "too-few" | "all-positive" | "all-negative" | "no-game-gap";

export interface AdoptResult {
  adopted: PoolPersona[];
  rejected: Array<{ speakerId: string; reason: RejectReason; count: number }>;
}

function mean(vectors: number[][]): number[] {
  const out = new Array<number>(DIM).fill(0);
  if (vectors.length === 0) return out;
  for (const v of vectors) for (let d = 0; d < DIM; d++) out[d] += v[d];
  return out.map((x) => x / vectors.length);
}

function weightedMean(vectors: number[][], weights: number[]): number[] {
  const wsum = weights.reduce((s, w) => s + w, 0) || 1;
  const out = new Array<number>(DIM).fill(0);
  vectors.forEach((v, i) => {
    for (let d = 0; d < DIM; d++) out[d] += (v[d] * weights[i]) / wsum;
  });
  return out.map((x) => +x.toFixed(4));
}

function euclid(a: number[], b: number[]): number {
  let s = 0;
  for (let i = 0; i < a.length; i++) s += (a[i] - b[i]) ** 2;
  return Math.sqrt(s);
}

/** 採用判定 (純粋関数)。プール書込はしない。採用候補と却下理由を返す。 */
export function evaluateSpeakers(
  speakers: SpeakerOpinions[],
  opts: AdoptOptions = {}
): {
  candidates: Array<{ speakerId: string; source?: string; affect: number[]; count: number }>;
  rejected: AdoptResult["rejected"];
} {
  const minOpinions = opts.minOpinions ?? 10;
  const eps = opts.polarityEps ?? 0.05;
  const gapEps = opts.gameGapEps ?? 0.05;

  const candidates: Array<{ speakerId: string; source?: string; affect: number[]; count: number }> = [];
  const rejected: AdoptResult["rejected"] = [];

  for (const sp of speakers) {
    const count = sp.opinions.length;
    if (count < minOpinions) {
      rejected.push({ speakerId: sp.speakerId, reason: "too-few", count });
      continue;
    }
    const vecs = sp.opinions.map((o) => textToVector(o.text));
    // polarity (valence = 次元 0, 0.5 が中立)。
    const pos = vecs.filter((v) => v[0] > 0.5 + eps).length;
    const neg = vecs.filter((v) => v[0] < 0.5 - eps).length;
    if (pos === count) {
      rejected.push({ speakerId: sp.speakerId, reason: "all-positive", count });
      continue;
    }
    if (neg === count) {
      rejected.push({ speakerId: sp.speakerId, reason: "all-negative", count });
      continue;
    }
    // ゲーム間 gap: per-game 平均ベクトルが ≥2 ゲームで存在し、最大対距離 > gapEps。
    const byGame = new Map<string, number[][]>();
    sp.opinions.forEach((o, i) => {
      const key = o.gameSlug ?? "_none";
      (byGame.get(key) ?? byGame.set(key, []).get(key)!).push(vecs[i]);
    });
    const gameMeans = [...byGame.values()].map((vs) => mean(vs));
    let maxGap = 0;
    for (let i = 0; i < gameMeans.length; i++)
      for (let j = i + 1; j < gameMeans.length; j++) maxGap = Math.max(maxGap, euclid(gameMeans[i], gameMeans[j]));
    if (gameMeans.length < 2 || maxGap <= gapEps) {
      rejected.push({ speakerId: sp.speakerId, reason: "no-game-gap", count });
      continue;
    }
    // affect = 本人意見の重み付け平均 (偏りを保持)。
    const weights = sp.opinions.map((o) => o.weight ?? 1);
    candidates.push({ speakerId: sp.speakerId, source: sp.source, affect: weightedMean(vecs, weights), count });
  }
  return { candidates, rejected };
}

/**
 * 話者意見群を評価し、採用候補をプールへ upsert する (C1-a)。
 * typicality = 既存採用 + 今回候補の母集団平均への cosine。
 */
export function adoptPersonas(speakers: SpeakerOpinions[], opts: AdoptOptions = {}): AdoptResult {
  const { candidates, rejected } = evaluateSpeakers(speakers, opts);

  // 母集団平均 = 既存 adopted + 今回候補の affect 平均 → typicality (cosine) を全 adopted で再計算。
  const existing = listPoolPersonas({ origin: "adopted" });
  const popMean = mean([...existing.map((p) => p.affectVector), ...candidates.map((c) => c.affect)]);

  const adopted: PoolPersona[] = [];
  for (const c of candidates) {
    const persona: PoolPersona = {
      id: "", // upsert が既存 id を優先、新規は呼び出し側で採番不要 (insert 側が使う)
      name: maskedPersonaLabel(c.speakerId),
      role: "opinion",
      speechStyle: "", // 口調は付けない (C1)
      traits: [],
      affectVector: c.affect,
      origin: "adopted",
      parentIds: [],
      learningSource: c.source,
      sourceSpeakerId: c.speakerId,
      typicality: +cosine(c.affect, popMean).toFixed(4),
    };
    persona.id = randomUUID();
    adopted.push(upsertPoolPersonaBySpeaker(persona));
  }
  // 既存 adopted の typicality も新しい母集団平均で更新 (一貫性)。
  for (const p of existing) setPoolPersonaTypicality(p.id, +cosine(p.affectVector, popMean).toFixed(4));

  return { adopted, rejected };
}
