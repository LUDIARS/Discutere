import { NEGATIVE_AFFECTS } from "./affect-negatives.js";

export interface MatchInput {
  intended: string;
  observed: string[];
}

export function shouldDetectGap(input: MatchInput): { detect: boolean; type: "missing" | "negative" | "mixed" | "none" } {
  if (!input.observed.length) return { detect: true, type: "missing" };
  const hasIntended = input.observed.includes(input.intended);
  if (!hasIntended) return { detect: true, type: "missing" };

  // 負感情は Affect 語彙の valence === "negative" を参照 (affect-negatives.ts)
  const negatives = input.observed.filter((x) => NEGATIVE_AFFECTS.has(x));
  if (negatives.length > 0) return { detect: true, type: "negative" };

  const uniq = Array.from(new Set(input.observed));
  if (uniq.length > 1) return { detect: true, type: "mixed" };
  return { detect: false, type: "none" };
}
