/**
 * Gap率アナライザ (read-only)。
 *
 * 「運営の想定感情」(GameKG md の mechanic 単位 intended 定義) と「観測感情」(KG 発話を
 * lexicon 採点) を突合し、**運営の意図と違う感情がどれだけ出ているか = Gap率** を
 * 施策時系列 / ソース / 感情次元 / mechanic の多角で算出する。Bot 不要・現データで動く。
 *
 *   intended = data/games/<slug>.md  (運営の定義: mechanic.intended_valence / intended_aspects)
 *   observed = KG utterances → scoreText() で valence/aspect 採点
 */

import fs from "node:fs";
import path from "node:path";

import Database from "better-sqlite3";

import { parseGameKG } from "../crawler/md-format.js";
import type { MechanicEntry } from "../crawler/types.js";
import { getConfig } from "../config.js";
import { resolveActiveKgPath } from "../core/kg-registry.js";
import { scoreText, type AffectScore, type ValenceLabel } from "./affect-score.js";

/** 発話 1 件の運営想定に対する分類。 */
export type GapClass = "match" | "offIntentPositive" | "unintendedNegative" | "blindSpot" | "neutral";

export interface MechanicGap {
  name: string;
  intendedValence: ValenceLabel;
  intendedAspects: string[];
  attributed: number;
  gapRate: number; // 帰属発話のうち運営想定と不一致の割合
  negativeRate: number;
}

export interface SliceGap {
  key: string;
  n: number;
  gapRate: number;
  negativeRate: number;
}

export interface AspectGap {
  aspect: string;
  intended: boolean; // どれかの mechanic が想定している aspect か
  mentions: number;
  negativeRate: number; // その話題で負感情が出ている割合 = 運営と違う度
}

export interface GapRateReport {
  game: string;
  slug: string;
  total: number;
  breakdown: Record<GapClass, number>;
  matchRate: number;
  gapRate: number; // = 1 - matchRate (中立を含む母数。下に neutralRate)
  neutralRate: number;
  negativeRate: number; // 運営の狙い(ポジ体験)と無関係に負感情が出ている割合
  bySource: SliceGap[];
  byPeriod: SliceGap[];
  byAspect: AspectGap[];
  byMechanic: MechanicGap[];
}

interface ScoredUtterance {
  source: string;
  period: string; // YYYY-MM
  score: AffectScore;
  cls: GapClass;
}

interface IntendedDef {
  intendedAspects: Set<string>;
  mechanics: Array<{ name: string; valence: ValenceLabel; aspects: string[] }>;
}

function kuzuPath(): string {
  // active KG (§12) を見る。 未解決時は従来既定に倒す (完全後方互換)。
  return resolveActiveKgPath(getConfig()) ?? process.env.DISCATIER_KUZU_PATH ?? path.resolve("./data/discatier.kuzu");
}

/** GameKG md (運営の定義) を読み、intended プロファイルを組む。 */
export function loadIntended(slug: string, gamesDir = path.resolve("./data/games")): IntendedDef {
  const mdPath = path.join(gamesDir, `${slug}.md`);
  if (!fs.existsSync(mdPath)) {
    throw new Error(`GameKG md が見つかりません: ${mdPath} (運営の想定感情を定義してください)`);
  }
  const kg = parseGameKG(fs.readFileSync(mdPath, "utf8"), mdPath);
  const intendedAspects = new Set<string>();
  const mechanics = kg.mechanics.map((m: MechanicEntry) => {
    const aspects = m.intended_aspects ?? [];
    for (const a of aspects) intendedAspects.add(a);
    return { name: m.name, valence: (m.intended_valence ?? "positive") as ValenceLabel, aspects };
  });
  return { intendedAspects, mechanics };
}

/** KG から該当ゲームの発話を取り出して採点する。session.title = `<source>:<slug>`。 */
export function loadObserved(slug: string, dbPath = kuzuPath()): ScoredUtterance[] {
  const db = new Database(dbPath, { readonly: true, fileMustExist: true });
  try {
    const sessions = db
      .prepare("SELECT id, title FROM sessions")
      .all() as Array<{ id: string; title: string }>;
    const mine = sessions.filter((s) => (s.title || "").endsWith(`:${slug}`));
    const bySession = new Map(mine.map((s) => [s.id, (s.title || "").split(":")[0] || "unknown"]));
    if (mine.length === 0) return [];

    const placeholders = mine.map(() => "?").join(",");
    const rows = db
      .prepare(
        `SELECT session_id, raw_content, posted_at FROM utterances WHERE session_id IN (${placeholders})`
      )
      .all(...mine.map((s) => s.id)) as Array<{
      session_id: string;
      raw_content: string;
      posted_at: number;
    }>;

    return rows.map((r) => {
      const source = bySession.get(r.session_id) ?? "unknown";
      const period = r.posted_at ? new Date(r.posted_at).toISOString().slice(0, 7) : "unknown";
      const score = scoreText(r.raw_content);
      return { source, period, score, cls: "neutral" as GapClass };
    });
  } finally {
    db.close();
  }
}

/** 発話 1 件を運営想定に対して分類する。 */
function classify(score: AffectScore, intended: IntendedDef): GapClass {
  if (score.valenceLabel === "negative") return "unintendedNegative"; // 運営=ポジ体験 と不一致
  const onIntent = score.aspectsHit.some((a) => intended.intendedAspects.has(a));
  if (score.valenceLabel === "positive") return onIntent ? "match" : "offIntentPositive";
  // neutral: 想定外 aspect への言及があれば blind-spot、無シグナルは neutral
  const offIntent = score.aspectsHit.some((a) => !intended.intendedAspects.has(a));
  return offIntent ? "blindSpot" : "neutral";
}

function rate(part: number, whole: number): number {
  return whole > 0 ? Math.round((part / whole) * 1000) / 1000 : 0;
}

function sliceGap(items: ScoredUtterance[], keyOf: (u: ScoredUtterance) => string): SliceGap[] {
  const groups = new Map<string, ScoredUtterance[]>();
  for (const u of items) {
    const k = keyOf(u);
    (groups.get(k) ?? groups.set(k, []).get(k)!).push(u);
  }
  return [...groups.entries()]
    .map(([key, us]) => {
      const signal = us.filter((u) => u.cls !== "neutral").length;
      const gap = us.filter((u) => u.cls !== "match" && u.cls !== "neutral").length;
      const neg = us.filter((u) => u.cls === "unintendedNegative").length;
      return { key, n: us.length, gapRate: rate(gap, signal), negativeRate: rate(neg, signal) };
    })
    .sort((a, b) => b.n - a.n);
}

/** Gap率レポートを組み立てる。 */
export function analyzeGapRate(slug: string, gamesDir?: string, dbPath?: string): GapRateReport {
  const intended = loadIntended(slug, gamesDir);
  const observed = loadObserved(slug, dbPath);
  for (const u of observed) u.cls = classify(u.score, intended);

  const total = observed.length;
  const breakdown: Record<GapClass, number> = {
    match: 0,
    offIntentPositive: 0,
    unintendedNegative: 0,
    blindSpot: 0,
    neutral: 0,
  };
  for (const u of observed) breakdown[u.cls] += 1;

  // Gap率は「感情シグナルのある発話」を母数にする (中立=無シグナルは別軸=カバレッジ)。
  const signal = total - breakdown.neutral;
  const matchRate = rate(breakdown.match, signal);
  const gapRate = signal > 0 ? Math.round((1 - matchRate) * 1000) / 1000 : 0;
  const negativeRate = rate(breakdown.unintendedNegative, signal);
  const neutralRate = rate(breakdown.neutral, total); // = 1 - カバレッジ

  // 感情次元軸: aspect ごとの負感情率
  const byAspect: AspectGap[] = [];
  const aspectKeys = new Set<string>();
  for (const u of observed) for (const a of u.score.aspectsHit) aspectKeys.add(a);
  for (const a of aspectKeys) {
    const ment = observed.filter((u) => u.score.aspects[a] > 0);
    const neg = ment.filter((u) => u.score.valenceLabel === "negative").length;
    byAspect.push({
      aspect: a,
      intended: intended.intendedAspects.has(a),
      mentions: ment.length,
      negativeRate: rate(neg, ment.length),
    });
  }
  byAspect.sort((x, y) => y.negativeRate - x.negativeRate || y.mentions - x.mentions);

  // mechanic 軸: aspect 重なりで帰属 → 想定 valence と不一致の割合
  const byMechanic: MechanicGap[] = intended.mechanics.map((m) => {
    const attr = observed.filter((u) => u.score.aspectsHit.some((a) => m.aspects.includes(a)));
    const mismatch = attr.filter((u) => u.score.valenceLabel !== m.valence).length;
    const neg = attr.filter((u) => u.score.valenceLabel === "negative").length;
    return {
      name: m.name,
      intendedValence: m.valence,
      intendedAspects: m.aspects,
      attributed: attr.length,
      gapRate: rate(mismatch, attr.length),
      negativeRate: rate(neg, attr.length),
    };
  });
  byMechanic.sort((x, y) => y.gapRate - x.gapRate);

  return {
    game: slug,
    slug,
    total,
    breakdown,
    matchRate,
    gapRate,
    neutralRate,
    negativeRate,
    bySource: sliceGap(observed, (u) => u.source),
    byPeriod: sliceGap(observed, (u) => u.period).sort((a, b) => a.key.localeCompare(b.key)),
    byAspect,
    byMechanic,
  };
}

const pct = (x: number): string => `${(x * 100).toFixed(1)}%`;

/** レポートを人間可読のテキストに整形する (CLI 用)。 */
export function formatGapReport(r: GapRateReport): string {
  const L: string[] = [];
  L.push(`=== Gap率レポート: ${r.slug} (総発話 ${r.total}) ===`);
  L.push(`感情シグナル率(カバレッジ) ${pct(1 - r.neutralRate)}  ※残りは中立/無シグナル`);
  L.push(
    `[シグナル中] 一致率 ${pct(r.matchRate)} / Gap率 ${pct(r.gapRate)} / ` +
      `意図外負(運営と違う) ${pct(r.negativeRate)}`
  );
  L.push(
    `内訳: 一致 ${r.breakdown.match} / 別正 ${r.breakdown.offIntentPositive} / ` +
      `意図外負 ${r.breakdown.unintendedNegative} / 未語彙化 ${r.breakdown.blindSpot} / 中立 ${r.breakdown.neutral}`
  );
  L.push("\n[ソース別] gap / neg");
  for (const s of r.bySource) L.push(`  ${s.key.padEnd(10)} n=${String(s.n).padStart(5)}  gap ${pct(s.gapRate)} / neg ${pct(s.negativeRate)}`);
  L.push("\n[施策/月次] gap / neg");
  for (const p of r.byPeriod) L.push(`  ${p.key.padEnd(10)} n=${String(p.n).padStart(5)}  gap ${pct(p.gapRate)} / neg ${pct(p.negativeRate)}`);
  L.push("\n[感情次元] 言及あたり負率 = 運営と違う度 (★=運営想定aspect)");
  for (const a of r.byAspect.slice(0, 10)) L.push(`  ${a.intended ? "★" : " "} ${a.aspect.padEnd(13)} 言及 ${String(a.mentions).padStart(5)}  neg ${pct(a.negativeRate)}`);
  L.push("\n[mechanic別] 想定 vs 観測の不一致率");
  for (const m of r.byMechanic.slice(0, 12)) L.push(`  ${m.name.slice(0, 22).padEnd(22)} 想定${m.intendedValence.padEnd(8)} 帰属${String(m.attributed).padStart(5)}  gap ${pct(m.gapRate)} / neg ${pct(m.negativeRate)}`);
  return L.join("\n");
}
