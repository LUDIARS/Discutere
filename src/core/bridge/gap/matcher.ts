export interface MatchInput {
  intended: string;
  observed: string[];
}

export function shouldDetectGap(input: MatchInput): { detect: boolean; type: "missing" | "negative" | "mixed" | "none" } {
  if (!input.observed.length) return { detect: true, type: "missing" };
  const hasIntended = input.observed.includes(input.intended);
  if (!hasIntended) return { detect: true, type: "missing" };

  const negatives = input.observed.filter((x) => ["frustration", "anger", "sadness", "boredom"].includes(x));
  if (negatives.length > 0) return { detect: true, type: "negative" };

  const uniq = Array.from(new Set(input.observed));
  if (uniq.length > 1) return { detect: true, type: "mixed" };
  return { detect: false, type: "none" };
}
