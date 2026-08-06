import type { PoolPersona } from "./persona-pool.js";

// @spec 憑依 descriptor の強化
const AXIS_LABELS: Record<string, string> = {
  "mtg.timmy": "大きな体験",
  "mtg.johnny": "独自の工夫",
  "mtg.spike": "勝利と最適化",
  "style.achiever": "達成",
  "style.explorer": "探索",
  "style.socializer": "交流",
  "style.competitor": "競争",
  "style.collector": "収集",
  "style.narrative": "物語",
  "style.relaxation": "くつろぎ",
  "style.mastery": "上達",
  "style.onboarding_need": "丁寧な導入",
  "style.autonomy": "自由度",
  "style.routine_tolerance": "反復",
  "style.monetization_sensitivity": "課金への慎重さ",
};

function axisLabel(axis: string): string {
  return AXIS_LABELS[axis] ?? axis;
}

export function buildPersonaDescriptor(persona: PoolPersona): string {
  const parts: string[] = [];
  const axes = Object.entries(persona.preferenceAxes ?? {})
    .filter(([, score]) => Number.isFinite(score))
    .sort(([leftId, left], [rightId, right]) => right - left || leftId.localeCompare(rightId));
  if (axes.length > 0) {
    const high = axes.slice(0, 2).map(([axis]) => axisLabel(axis));
    parts.push(`${high.join("と")}を強く好む`);
    const [lowAxis] = axes[axes.length - 1];
    if (!high.includes(axisLabel(lowAxis))) parts.push(`${axisLabel(lowAxis)}は好まない`);
  }

  const aversions = [...(persona.aversions ?? [])]
    .sort((left, right) =>
      right.strength - left.strength || left.target.localeCompare(right.target))
    .slice(0, 2)
    .map((item) => item.target.replace(/^mechanic:/, ""));
  if (aversions.length > 0) parts.push(`${aversions.join("・")}を嫌う`);

  const attributes = [
    persona.attributes?.ageBand,
    persona.attributes?.spending,
  ].filter(Boolean);
  if (attributes.length > 0) parts.push(attributes.join(" / "));

  const reactions = [...(persona.mechanicReactions ?? [])]
    .sort((left, right) =>
      Math.abs(right.sentiment) - Math.abs(left.sentiment)
      || left.mechanicId.localeCompare(right.mechanicId))
    .slice(0, 2)
    .map((item) => `${item.mechanicId}${item.sentiment >= 0 ? "に強く反応" : "が苦手"}`);
  if (reactions.length > 0) parts.push(reactions.join("、"));

  if (parts.length === 0) {
    if (persona.traits.length > 0) parts.push(`特徴: ${persona.traits.join(" / ")}`);
    if (persona.speechStyle) parts.push(`話し方: ${persona.speechStyle}`);
    if (persona.learningSource) parts.push(`出所: ${persona.learningSource}`);
    if (typeof persona.typicality === "number") {
      parts.push(persona.typicality >= 0.9 ? "多数派寄りの感性" : "やや独自の感性");
    }
    if (typeof persona.polarityBias === "number") {
      parts.push(persona.polarityBias >= 0.5 ? "好き嫌いがはっきりしている" : "是々非々で評価する");
    }
    if (typeof persona.affectDispersion === "number" && persona.affectDispersion >= 0.1) {
      parts.push("ゲームによって評価が大きく変わる");
    }
  }

  return parts.length > 0
    ? `この人物像は実データ由来。${parts.join("、")}。この感性になりきって発言する。`
    : "この人物像は実データ由来の感性を持つ。その立場になりきって発言する。";
}
