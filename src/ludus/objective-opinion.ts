interface InputMessage {
  authorName: string;
  text: string;
}

interface MechanicRow {
  mechanic: string;
  confidence: number;
  gameTitle: string;
}

export function buildObjectiveOpinion(input: {
  question: string;
  messages: InputMessage[];
  mechanics: MechanicRow[];
}): string {
  const q = input.question.trim();
  const recent = input.messages.slice(-20);
  const joined = `${q}\n${recent.map((m) => m.text).join("\n")}`.toLowerCase();

  const ranked = rankMechanics(input.mechanics, joined).slice(0, 6);
  const risks = detectRisks(joined);
  const recommendations = buildRecommendations(ranked, risks);

  const lines: string[] = [];
  lines.push("Objective Opinion");
  lines.push(`Question: ${q}`);
  lines.push("");
  lines.push("Mechanics signals:");
  if (ranked.length === 0) {
    lines.push("- No relevant mechanics were matched in dictionary.");
  } else {
    for (const r of ranked) {
      lines.push(`- ${r.mechanic} (score=${r.score}, confidence=${r.confidence})`);
    }
  }
  lines.push("");
  lines.push("Assessment:");
  lines.push(...recommendations.map((x) => `- ${x}`));
  lines.push("");
  lines.push("Decision frame:");
  lines.push("- Keep only mechanics that reinforce core loop in first 30-60 seconds.");
  lines.push("- Remove mechanics that require explanation longer than one sentence.");
  lines.push("- Prioritize readability and feedback over feature count.");
  return lines.join("\n");
}

function rankMechanics(mechanics: MechanicRow[], context: string): Array<{ mechanic: string; confidence: number; score: number }> {
  const out: Array<{ mechanic: string; confidence: number; score: number }> = [];
  const seen = new Set<string>();
  for (const m of mechanics) {
    const key = m.mechanic.toLowerCase();
    if (seen.has(key)) continue;
    seen.add(key);
    const hits = countHits(context, key);
    if (hits <= 0) continue;
    out.push({
      mechanic: m.mechanic,
      confidence: m.confidence,
      score: m.confidence + hits * 10,
    });
  }
  return out.sort((a, b) => b.score - a.score);
}

function countHits(text: string, keyword: string): number {
  let count = 0;
  let idx = 0;
  while (true) {
    const found = text.indexOf(keyword, idx);
    if (found < 0) break;
    count += 1;
    idx = found + keyword.length;
  }
  return count;
}

function detectRisks(context: string): string[] {
  const risks: string[] = [];
  if (/(too many|複雑|むずか|難し|overwhelm)/i.test(context)) risks.push("complexity overload");
  if (/(slow|遅い|テンポ|pace)/i.test(context)) risks.push("low pacing");
  if (/(balance|壊れ|op|弱い|強すぎ)/i.test(context)) risks.push("balance volatility");
  if (/(grind|作業|単調)/i.test(context)) risks.push("repetition fatigue");
  return risks;
}

function buildRecommendations(
  ranked: Array<{ mechanic: string; confidence: number; score: number }>,
  risks: string[],
): string[] {
  const rec: string[] = [];
  if (ranked.length > 0) {
    rec.push(`Focus on top mechanics: ${ranked.slice(0, 3).map((x) => x.mechanic).join(", ")}.`);
  } else {
    rec.push("Current discussion has weak linkage to known mechanics; define explicit core loop first.");
  }
  if (risks.includes("complexity overload")) {
    rec.push("Cut one subsystem before adding new progression layers.");
  }
  if (risks.includes("low pacing")) {
    rec.push("Increase decision frequency; target one meaningful choice every 10-20 seconds.");
  }
  if (risks.includes("balance volatility")) {
    rec.push("Constrain multiplicative synergies and add hard caps early.");
  }
  if (risks.includes("repetition fatigue")) {
    rec.push("Introduce variation triggers tied to player state, not random-only events.");
  }
  if (rec.length === 1) {
    rec.push("Run two prototypes with opposite tuning and compare retention-driving moments.");
  }
  return rec;
}

