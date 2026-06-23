/**
 * ① ユーザ嗜好データの蓄積 (擬似集団)。
 *
 * data/games/*.sentiment.json を全走査し、genre / mechanic 粒度に集約した
 * PreferenceProfile を生成する。個人データは保持しない (集団のみ)。
 *
 * spec/plan/RESTRUCTURE.md §2-① / §4-A。
 */

import { readdir, readFile, writeFile, mkdir } from "node:fs/promises";
import { join } from "node:path";
import type { PreferenceProfile, SentimentVector, Step1Output } from "./types.js";

const VECTOR_DIM = 20;
const ZERO_VECTOR: SentimentVector = Array(VECTOR_DIM).fill(0) as SentimentVector;

// ── sentinel 型 ─────────────────────────────────────────────────────────────

interface SentimentSidecar {
  game?: string;
  slug?: string;
  overall?: {
    vector?: number[];
    volume?: number;
    positive_ratio?: number;
    volume_log?: number;
  };
  sources?: Array<{ source: string; n?: number }>;
}

interface GameMdFrontmatter {
  genre?: string;
  mechanics?: Array<{ name: string }>;
}

// ── ベクトル演算 ──────────────────────────────────────────────────────────────

function addVectors(a: number[], b: number[], weight: number): number[] {
  return a.map((v, i) => v + (b[i] ?? 0) * weight);
}

function normalizeVector(v: number[], total: number): SentimentVector {
  return v.map((x) => (total > 0 ? x / total : 0)) as SentimentVector;
}

/** volume_log 重みづき加重平均 (spec §2-①) */
function weightedAverage(
  acc: { sum: number[]; totalWeight: number },
  vec: number[],
  weight: number,
): { sum: number[]; totalWeight: number } {
  return {
    sum: addVectors(acc.sum, vec, weight),
    totalWeight: acc.totalWeight + weight,
  };
}

// ── ファイル読込 ──────────────────────────────────────────────────────────────

async function loadSentimentFiles(gamesDir: string): Promise<
  Array<{ slug: string; sidecar: SentimentSidecar; frontmatter: GameMdFrontmatter }>
> {
  const entries = await readdir(gamesDir).catch(() => []);
  const results: Array<{ slug: string; sidecar: SentimentSidecar; frontmatter: GameMdFrontmatter }> = [];

  for (const entry of entries) {
    if (!entry.endsWith(".sentiment.json")) continue;
    const slug = entry.replace(/\.sentiment\.json$/, "");
    try {
      const raw = await readFile(join(gamesDir, entry), "utf-8");
      const sidecar = JSON.parse(raw) as SentimentSidecar;

      // 対応 md から frontmatter を読む
      const mdPath = join(gamesDir, `${slug}.md`);
      const mdRaw = await readFile(mdPath, "utf-8").catch(() => "");
      const frontmatter = parseFrontmatter(mdRaw);

      results.push({ slug, sidecar, frontmatter });
    } catch {
      // 読み込み失敗は skip
    }
  }

  return results;
}

/** YAML frontmatter から genre / mechanics だけ取り出す簡易パーサ。CRLF/LF 両対応。 */
function parseFrontmatter(mdContent: string): GameMdFrontmatter {
  const content = mdContent.replace(/\r\n/g, "\n");
  const match = /^---\n([\s\S]*?)\n---/.exec(content);
  if (!match) return {};

  const lines = match[1].split("\n");
  let genre: string | undefined;
  const mechanics: Array<{ name: string }> = [];
  let inMechanics = false;

  for (const line of lines) {
    const scalar = (v: string) => v.replace(/^["']|["']$/g, "").trim();
    const topKey = /^(\w[\w_]*):\s*(.*)/.exec(line);
    if (topKey && !line.startsWith(" ") && !line.startsWith("\t")) {
      inMechanics = topKey[1] === "mechanics";
      if (topKey[1] === "genre") genre = scalar(topKey[2]);
      continue;
    }
    if (inMechanics) {
      const nameMatch = /^\s{2}-\s+name:\s*(.+)/.exec(line);
      if (nameMatch) mechanics.push({ name: scalar(nameMatch[1]) });
    }
  }

  return { genre, mechanics: mechanics.length > 0 ? mechanics : undefined };
}

/** sentiment sidecar から 20 次元ベクトルを復元。overall.vector があればそれを使う。 */
function extractVector(sidecar: SentimentSidecar): number[] | null {
  const v = sidecar.overall?.vector;
  if (Array.isArray(v) && v.length === VECTOR_DIM) return v;

  // overall に scalar しかない場合: partial reconstruction
  const overall = sidecar.overall;
  if (!overall) return null;

  const vec = Array(VECTOR_DIM).fill(0.5) as number[];
  // meta フィールドだけ確実に入れる
  if (typeof overall.positive_ratio === "number") vec[18] = overall.positive_ratio;
  if (typeof overall.volume_log === "number") vec[19] = overall.volume_log;
  return vec;
}

// ── 集約 ──────────────────────────────────────────────────────────────────────

export async function buildPreferences(gamesDir: string): Promise<Step1Output> {
  const entries = await loadSentimentFiles(gamesDir);
  if (entries.length === 0) {
    return { profiles: [] };
  }

  // genre 別 accumulator
  const genreAcc = new Map<string, { sum: number[]; totalWeight: number; sources: string[] }>();
  // mechanic 別 accumulator
  const mechanicAcc = new Map<string, { sum: number[]; totalWeight: number; sources: string[] }>();
  // global accumulator
  let globalAcc = { sum: Array(VECTOR_DIM).fill(0) as number[], totalWeight: 0 };
  const globalSources: string[] = [];

  for (const { slug, sidecar, frontmatter } of entries) {
    const vec = extractVector(sidecar);
    if (!vec) continue;

    const volumeLog = sidecar.overall?.volume_log ?? 0.3;
    const weight = volumeLog;

    // 出所サマリ (source 種別のみ。個人アンカーは保持しない)
    const sourceLabel = sidecar.sources?.map((s) => s.source).join(",") ?? "unknown";

    // global
    globalAcc = weightedAverage(globalAcc, vec, weight);
    globalSources.push(`${slug}(${sourceLabel})`);

    // genre
    if (frontmatter.genre) {
      const key = `genre:${frontmatter.genre}`;
      const prev = genreAcc.get(key) ?? { sum: Array(VECTOR_DIM).fill(0) as number[], totalWeight: 0, sources: [] };
      const next = weightedAverage(prev, vec, weight);
      genreAcc.set(key, { ...next, sources: [...prev.sources, `${slug}(${sourceLabel})`] });
    }

    // mechanic 別
    for (const mech of frontmatter.mechanics ?? []) {
      const key = `mechanic:${mech.name}`;
      const prev = mechanicAcc.get(key) ?? { sum: Array(VECTOR_DIM).fill(0) as number[], totalWeight: 0, sources: [] };
      const next = weightedAverage(prev, vec, weight);
      mechanicAcc.set(key, { ...next, sources: [...prev.sources, `${slug}(${sourceLabel})`] });
    }
  }

  const profiles: PreferenceProfile[] = [];

  // global
  profiles.push({
    scope: "global",
    vector: normalizeVector(globalAcc.sum, globalAcc.totalWeight),
    sampleCount: entries.length,
    sources: globalSources,
  });

  // genre
  for (const [scope, acc] of genreAcc) {
    profiles.push({
      scope,
      vector: normalizeVector(acc.sum, acc.totalWeight),
      sampleCount: acc.sources.length,
      sources: acc.sources,
    });
  }

  // mechanic (サンプル数が少ない場合も保持するが sources でわかる)
  for (const [scope, acc] of mechanicAcc) {
    profiles.push({
      scope,
      vector: normalizeVector(acc.sum, acc.totalWeight),
      sampleCount: acc.sources.length,
      sources: acc.sources,
    });
  }

  return { profiles };
}

/** data/preferences/<scope>.json に保存 */
export async function savePreferences(prefsDir: string, output: Step1Output): Promise<void> {
  await mkdir(prefsDir, { recursive: true });
  for (const profile of output.profiles) {
    const filename = profile.scope.replace(/[^a-z0-9_.-]/gi, "_") + ".json";
    await writeFile(join(prefsDir, filename), JSON.stringify(profile, null, 2), "utf-8");
  }
}

/** scope に一致するプロファイルを返す。無ければ global にフォールバック。 */
export function findProfile(profiles: PreferenceProfile[], scope: string): PreferenceProfile | undefined {
  return profiles.find((p) => p.scope === scope) ?? profiles.find((p) => p.scope === "global");
}

/** cosine 類似度 (0..1) */
export function cosine(a: number[], b: number[]): number {
  let dot = 0, normA = 0, normB = 0;
  for (let i = 0; i < a.length; i++) {
    dot += (a[i] ?? 0) * (b[i] ?? 0);
    normA += (a[i] ?? 0) ** 2;
    normB += (b[i] ?? 0) ** 2;
  }
  const denom = Math.sqrt(normA) * Math.sqrt(normB);
  return denom === 0 ? 0 : dot / denom;
}

export { ZERO_VECTOR };
