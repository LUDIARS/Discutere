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
import matter from "gray-matter";
import { ASP_KEYS, EMO_KEYS, DIM, VECTOR_DIMS, subtract } from "./sentiment-vector.js";
import { buildAliasGroups, expandAliases } from "../discatier-engine-adapter/game-aliases.js";
import { LUDUS_TERM_ALIAS_GROUPS } from "../discatier-engine-adapter/ludus-term-aliases.js";
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
 * gamesDir からゲームタイトル群を集める (別名グループ構築の材料)。
 *   - `*.md` の frontmatter title (例 "モンスターストライク" / "Genshin Impact (原神)")
 *   - `*.sentiment.json` の "game" フィールド (例 "Journey")
 * 読めないファイルは無視する (best-effort)。
 */
function collectGameTitles(gamesDir: string): string[] {
  const titles: string[] = [];
  for (const f of fs.readdirSync(gamesDir)) {
    const full = path.join(gamesDir, f);
    try {
      if (f.endsWith(".sentiment.json")) {
        const data = JSON.parse(fs.readFileSync(full, "utf-8")) as { game?: unknown };
        if (typeof data.game === "string" && data.game.trim()) titles.push(data.game.trim());
      } else if (f.endsWith(".md")) {
        const { data } = matter(fs.readFileSync(full, "utf-8"));
        if (typeof data.title === "string" && data.title.trim()) titles.push(data.title.trim());
      }
    } catch {
      // 読めないファイルは無視
    }
  }
  return titles;
}

/**
 * テーマに対応する `*.sentiment.json` の game_vector を現状ベクトルとして読む。
 * テーマ語は game-aliases (`expandAliases`) で別名展開してから照合する —
 * 日本語タイトル・口語略称 (例「モンスト」→「モンスターストライク」) でも届く (respec 07 §4)。
 * 見つからなければ warn を出して null (呼び出し側が negativeBaselineVector へフォールバック)。
 */
export function loadCurrentVector(
  theme: string,
  gamesDir = "./data/games",
  opts: { warn?: (msg: string) => void } = {}
): number[] | null {
  const warn = opts.warn ?? (() => {});
  if (!fs.existsSync(gamesDir)) {
    warn(`現状ベクトル: gamesDir が存在しません (${gamesDir})`);
    return null;
  }
  const files = fs.readdirSync(gamesDir).filter((f) => f.endsWith(".sentiment.json"));
  const words = theme
    .toLowerCase()
    .split(/\s+/)
    .filter((w) => w.length >= 2);
  // 別名展開: games タイトル (md frontmatter / sentiment.json) + 静的略称辞書。
  const terms = expandAliases(words, buildAliasGroups(collectGameTitles(gamesDir), LUDUS_TERM_ALIAS_GROUPS));

  for (const f of files) {
    const lower = f.toLowerCase();
    if (!terms.some((t) => lower.includes(t))) continue;
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
  warn(
    `現状ベクトル: テーマ「${theme}」に一致する sentiment.json が見つかりません` +
      ` (検索語: ${terms.join(", ") || "(なし)"})`
  );
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
