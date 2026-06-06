/**
 * 観測 affect 採点 (Gap率分析用)。
 *
 * 発話テキストを sentiment lexicon で採点し、valence / 感情(Plutchik 8) / aspect(8) の
 * 観測プロファイルを返す。`src/crawler/sentiment/analyze.mjs` の features() と同じ語彙・同じ
 * 判定を TS で再実装したもの (LLM 不要・決定的)。
 */

import fs from "node:fs";

interface Lexicon {
  polarity: Record<string, number>;
  emotions: Record<string, string[]>;
  aspects: Record<string, string[]>;
  arousal: string[];
}

const lex = JSON.parse(
  fs.readFileSync(new URL("../crawler/sentiment/lexicon.json", import.meta.url), "utf8")
) as Lexicon;

export const EMOTIONS = Object.keys(lex.emotions);
export const ASPECTS = Object.keys(lex.aspects);

export type ValenceLabel = "positive" | "negative" | "neutral";

export interface AffectScore {
  /** -1..1 */
  valence: number;
  valenceLabel: ValenceLabel;
  /** ヒットした感情 (Plutchik) */
  emotions: string[];
  /** aspect → ヒット語数 */
  aspects: Record<string, number>;
  /** ヒットした aspect 名のみ */
  aspectsHit: string[];
  /** 0..1 */
  arousal: number;
}

function valenceLabelOf(v: number): ValenceLabel {
  return v > 0.05 ? "positive" : v < -0.05 ? "negative" : "neutral";
}

/** 1 発話テキスト → 観測 affect プロファイル (決定的)。 */
export function scoreText(text: string): AffectScore {
  const t = (text || "").toLowerCase();
  const hit = (words: string[]): number =>
    words.reduce((s, w) => s + (t.includes(w.toLowerCase()) ? 1 : 0), 0);

  let pol = 0;
  let polHits = 0;
  for (const [w, sc] of Object.entries(lex.polarity)) {
    if (t.includes(w.toLowerCase())) {
      pol += sc;
      polHits += 1;
    }
  }
  const valence = polHits ? Math.max(-1, Math.min(1, pol / Math.max(2, polHits))) : 0;

  const emotions = EMOTIONS.filter((e) => hit(lex.emotions[e]) > 0);
  const aspects: Record<string, number> = {};
  for (const a of ASPECTS) aspects[a] = hit(lex.aspects[a]);
  const aspectsHit = ASPECTS.filter((a) => aspects[a] > 0);
  const arousal = Math.max(0, Math.min(1, (hit(lex.arousal) + ((text || "").split("!").length - 1)) / 4));

  return { valence, valenceLabel: valenceLabelOf(valence), emotions, aspects, aspectsHit, arousal };
}
