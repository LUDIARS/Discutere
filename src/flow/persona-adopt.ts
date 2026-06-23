/**
 * 実在ユーザ採用 (C1)。
 *
 * クロール済みの外部発話を話者アンカー (`ext:<source>:<authorId>`) で集約し、
 * 「意思ある人 (ゲーム嗜好あり)」をペルソナとしてプールに採用する。
 *
 * 採用条件 (全て満たす, spec/feature/flow/persona-pool.md §C1):
 *  - 意見が全ゲーム横断で minOpinions (既定 10) 以上。
 *  - 全ネガ / 全ポジを除外 (polarity が片寄りきっている人は弾く)。
 *  - ゲーム間で affect の差分 (gap) がある (≥2 ゲーム + per-game 連結ベクトルが同一でない)。
 *
 * affect 解像度向上 (#125, 非 embedding): 短文は **ゲーム単位で連結してからベクトル化** (粒度) し、
 * per-game ベクトルを **エンゲージメント (いいね = opinion-score) 重み付け平均** して affect とする。
 * 母集団平均からの近さ (typicality) に加え、極性偏り (polarity_bias) と ゲーム間ばらつき
 * (affect_dispersion) を追加特徴量として記録し、中立に潰れたベクトル同士の分離を補う。
 * 口調は付けない (LLM 不要 = 安価)。露出名は `論者#xxxxxx` (個人データ方針)。
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
  /** weight = opinion-score (エンゲージメント = いいね数 + 1)。未指定は 1 (等重み)。 */
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

/** 採用候補 (純粋関数の出力)。affect + 解像度向上のための追加特徴量 (#125)。 */
export interface AdoptCandidate {
  speakerId: string;
  source?: string;
  affect: number[];
  count: number;
  /** 極性の片寄り |pos-neg|/total (0=均衡 / 1=一方向)。 */
  polarityBias: number;
  /** ゲーム間 affect のばらつき (per-game ベクトルの平均対距離)。 */
  affectDispersion: number;
}

/**
 * 採用判定 (純粋関数)。プール書込はしない。採用候補と却下理由を返す。
 *
 * affect 解像度向上 (#125, 非 embedding):
 *  - (b) 粒度: 短文を **ゲーム単位で連結してから 1 回ベクトル化** (per-comment の中立丸まりを緩和)。
 *  - (a) opinion-score 重み付け: per-game ベクトルを **エンゲージメント (いいね) 合算**で重み付け平均。
 *  - (c) 追加特徴量: polarity_bias (極性偏り) と affect_dispersion (ゲーム間ばらつき) を算出。
 * 極性・gap 判定も per-game 連結ベクトルで行う (中立に潰れた per-comment valence より分離が効く)。
 */
export function evaluateSpeakers(
  speakers: SpeakerOpinions[],
  opts: AdoptOptions = {}
): {
  candidates: AdoptCandidate[];
  rejected: AdoptResult["rejected"];
} {
  const minOpinions = opts.minOpinions ?? 10;
  const eps = opts.polarityEps ?? 0.05;
  const gapEps = opts.gameGapEps ?? 0.05;

  const candidates: AdoptCandidate[] = [];
  const rejected: AdoptResult["rejected"] = [];

  for (const sp of speakers) {
    const count = sp.opinions.length;
    if (count < minOpinions) {
      rejected.push({ speakerId: sp.speakerId, reason: "too-few", count });
      continue;
    }
    // (b) ゲーム単位で本文連結 + (a) エンゲージメント (opinion-score = いいね) を合算。
    const byGame = new Map<string, { texts: string[]; weight: number }>();
    for (const o of sp.opinions) {
      const key = o.gameSlug ?? "_none";
      const g = byGame.get(key) ?? { texts: [], weight: 0 };
      g.texts.push(o.text);
      g.weight += o.weight ?? 1;
      byGame.set(key, g);
    }
    const games = [...byGame.values()];
    // ゲームを区別している = ≥2 ゲーム必須。
    if (games.length < 2) {
      rejected.push({ speakerId: sp.speakerId, reason: "no-game-gap", count });
      continue;
    }
    // 連結本文を 1 回ずつベクトル化 (短文の中立丸まりを緩和)。
    const gameVecs = games.map((g) => textToVector(g.texts.join(" \n ")));
    const gameWeights = games.map((g) => g.weight);

    // polarity は per-game ベクトルの valence (次元 0, 0.5 中立) で判定。
    const total = gameVecs.length;
    const pos = gameVecs.filter((v) => v[0] > 0.5 + eps).length;
    const neg = gameVecs.filter((v) => v[0] < 0.5 - eps).length;
    if (pos === total) {
      rejected.push({ speakerId: sp.speakerId, reason: "all-positive", count });
      continue;
    }
    if (neg === total) {
      rejected.push({ speakerId: sp.speakerId, reason: "all-negative", count });
      continue;
    }
    // ゲーム間 gap (最大対距離) + dispersion (平均対距離) を同時に算出。
    let maxGap = 0;
    let sumPair = 0;
    let pairCount = 0;
    for (let i = 0; i < gameVecs.length; i++) {
      for (let j = i + 1; j < gameVecs.length; j++) {
        const d = euclid(gameVecs[i], gameVecs[j]);
        maxGap = Math.max(maxGap, d);
        sumPair += d;
        pairCount += 1;
      }
    }
    if (maxGap <= gapEps) {
      rejected.push({ speakerId: sp.speakerId, reason: "no-game-gap", count });
      continue;
    }
    candidates.push({
      speakerId: sp.speakerId,
      source: sp.source,
      // (a) affect = per-game ベクトルのエンゲージメント重み付け平均 (偏りを保持)。
      affect: weightedMean(gameVecs, gameWeights),
      count,
      // (c) 追加特徴量。
      polarityBias: +(Math.abs(pos - neg) / total).toFixed(4),
      affectDispersion: pairCount > 0 ? +(sumPair / pairCount).toFixed(4) : 0,
    });
  }
  return { candidates, rejected };
}

/**
 * 話者意見群を評価し、採用候補をプールへ upsert する (C1-a)。
 * typicality = 既存採用 + 今回候補の母集団平均への cosine。
 * polarity_bias / affect_dispersion (#125) も併せて保存する。
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
      polarityBias: c.polarityBias,
      affectDispersion: c.affectDispersion,
    };
    persona.id = randomUUID();
    adopted.push(upsertPoolPersonaBySpeaker(persona));
  }
  // 既存 adopted の typicality も新しい母集団平均で更新 (一貫性)。
  for (const p of existing) setPoolPersonaTypicality(p.id, +cosine(p.affectVector, popMean).toFixed(4));

  return { adopted, rejected };
}
