/**
 * score-life-script — 人生脚本アンケートの回答を採点する (references/life-script.md「採点」の手順どおり)。
 *
 * 入力: <回答 JSON> [設問 JSON]
 *   回答 JSON = { questionnaire, revision, answers: { 項目ID: 1..4 }, forcedChoice: { FC-n: "a".."e" } }
 *   飛ばした設問は書かない。設問 JSON は省略時 skills/creator-insight/data/life-script-questionnaire.ja.json。
 * 出力 (stdout, JSON): { lifeScript: { questionnaire, revision, answered, drivers, process, position, responseStyle } }
 *   特徴カードの questionnaire.lifeScript と同じ形。尺度得点が「未回答」なら null。
 * 上限: 入力 256KB。設問に無い ID・範囲外の値・設問の版の違いはエラー (黙って捨てない)。
 * ネットワークに出ない。LLM を呼ばない。同じ入力からは同じ出力。
 *
 * 実行: npm run insight:score-life-script -- <回答 JSON> [設問 JSON]
 */
import path from "node:path";
import { fileURLToPath } from "node:url";
import {
  failCli, forcedSection, InputError, isRecord, likertSections, loadQuestionnaire, readJsonFile, round,
  type LikertItem, type Questionnaire,
} from "./lib/questionnaire.js";
import { DEFAULT_QUESTIONNAIRE } from "./life-script-items.js";

export interface LifeScriptAnswers {
  questionnaire: string;
  revision: string;
  answers: Record<string, number>;
  forcedChoice: Record<string, string>;
}

type Confidence = "high" | "medium" | "low";
type Band = "positive" | "negative" | "border";

export interface LifeScriptResult {
  questionnaire: string;
  revision: string;
  answered: number;
  drivers: {
    scores: Record<string, number | null>;
    personMean: number | null;
    dominant: string[];
    forcedChoice: Record<string, number>;
    confidence: Record<string, Confidence>;
  };
  process: {
    scores: Record<string, number | null>;
    personMean: number | null;
    dominant: string[];
    confidence: Record<string, Confidence>;
  };
  position: { self: number | null; other: number | null; selfBand: Band | null; otherBand: Band | null };
  responseStyle: {
    rawMean: number | null;
    rawSd: number | null;
    oneSided: boolean;
    flat: boolean;
    reverseConflicts: Array<{ item: string; key: string }>;
  };
}

/** 回答 JSON の形と値を確かめる。 */
export function parseAnswers(raw: unknown, q: Questionnaire): LifeScriptAnswers {
  if (!isRecord(raw)) throw new InputError("answers: not an object");
  if (raw.questionnaire !== q.id || raw.revision !== q.revision) {
    throw new InputError(`answers: questionnaire/revision mismatch (expected ${q.id} ${q.revision})`);
  }
  const likertIds = new Set(likertSections(q).flatMap((s) => s.items.map((i) => i.id)));
  const answers = isRecord(raw.answers) ? raw.answers : {};
  for (const [id, v] of Object.entries(answers)) {
    if (!likertIds.has(id)) throw new InputError(`answers: unknown item ${id}`);
    if (!Number.isInteger(v) || (v as number) < 1 || (v as number) > 4) throw new InputError(`answers: ${id} must be 1..4`);
  }
  const fc = forcedSection(q);
  const forced = isRecord(raw.forcedChoice) ? raw.forcedChoice : {};
  for (const [id, v] of Object.entries(forced)) {
    const item = fc?.items.find((i) => i.id === id);
    if (!item) throw new InputError(`forcedChoice: unknown item ${id}`);
    if (typeof v !== "string" || !item.options.some((o) => o.id === v)) throw new InputError(`forcedChoice: ${id} has no option ${String(v)}`);
  }
  return {
    questionnaire: q.id, revision: q.revision,
    answers: answers as Record<string, number>, forcedChoice: forced as Record<string, string>,
  };
}

const mean = (xs: number[]): number => xs.reduce((a, b) => a + b, 0) / xs.length;
const EPS = 1e-9;

interface ScaleStat { score: number | null; conflicts: Array<{ item: string; key: string }> }

/** 1 尺度の得点 (逆転を直した回答済み項目の平均) と逆転の矛盾。 */
function scaleStat(items: LikertItem[], answers: Record<string, number>, q: Questionnaire): ScaleStat {
  const converted = items
    .filter((i) => answers[i.id] !== undefined)
    .map((i) => ({ item: i, value: i.reverse ? 5 - answers[i.id]! : answers[i.id]! }));
  if (converted.length < items.length * q.scoring.scaleScore.minAnsweredRatio - EPS) return { score: null, conflicts: [] };
  const conflicts: Array<{ item: string; key: string }> = [];
  for (const c of converted.filter((x) => x.item.reverse)) {
    const others = converted.filter((x) => x !== c).map((x) => x.value);
    if (others.length > 0 && Math.abs(c.value - mean(others)) >= q.scoring.responseStyle.reverseConflictGap - EPS) {
      conflicts.push({ item: c.item.id, key: c.item.key });
    }
  }
  return { score: mean(converted.map((x) => x.value)), conflicts };
}

interface AxisStat { raw: Record<string, number | null>; personMean: number | null; conflicts: Array<{ item: string; key: string }> }

function axisStat(keys: string[], items: LikertItem[], answers: Record<string, number>, q: Questionnaire): AxisStat {
  const raw: Record<string, number | null> = {};
  const conflicts: Array<{ item: string; key: string }> = [];
  for (const key of keys) {
    const s = scaleStat(items.filter((i) => i.key === key), answers, q);
    raw[key] = s.score;
    conflicts.push(...s.conflicts);
  }
  const answered = Object.values(raw).filter((v): v is number => v !== null);
  return { raw, personMean: answered.length > 0 ? mean(answered) : null, conflicts };
}

function dominantOf(stat: AxisStat, q: Questionnaire, flat: boolean): string[] {
  if (flat || stat.personMean === null) return [];
  const d = q.scoring.dominance;
  const pm = stat.personMean;
  return Object.entries(stat.raw)
    .filter((e): e is [string, number] => e[1] !== null)
    .filter(([, v]) => v >= d.minScore - EPS && v - pm >= d.minGapFromPersonMean - EPS)
    .sort((a, b) => b[1] - a[1])
    .slice(0, d.maxReported)
    .map(([k]) => k);
}

function roundScores(raw: Record<string, number | null>, digits: number): Record<string, number | null> {
  return Object.fromEntries(Object.entries(raw).map(([k, v]) => [k, v === null ? null : round(v, digits)]));
}

function band(v: number | null, q: Questionnaire): Band | null {
  if (v === null) return null;
  const p = q.scoring.position;
  if (v >= p.borderLow - EPS && v <= p.borderHigh + EPS) return "border";
  return v > p.midpoint ? "positive" : "negative";
}

const STEP_UP: Record<Confidence, Confidence> = { low: "medium", medium: "high", high: "high" };

export function scoreLifeScript(q: Questionnaire, a: LifeScriptAnswers): LifeScriptResult {
  const s = q.scoring;
  const digits = s.scaleScore.round;
  const sections = likertSections(q);
  const likertItems = sections.flatMap((sec) => sec.items);

  const rawValues = likertItems.map((i) => a.answers[i.id]).filter((v): v is number => v !== undefined);
  const rawMean = rawValues.length > 0 ? mean(rawValues) : null;
  const rawSd = rawMean === null ? null : Math.sqrt(mean(rawValues.map((v) => (v - rawMean) ** 2)));
  const rs = s.responseStyle;
  const oneSided = rawMean !== null && (rawMean >= rs.oneSidedHighMean - EPS || rawMean <= rs.oneSidedLowMean + EPS);
  const flat = rawSd !== null && rawSd < rs.flatSdBelow - EPS;

  const drivers = axisStat(s.axes.drivers, likertItems, a.answers, q);
  const process = axisStat(s.axes.process, likertItems, a.answers, q);
  const position = axisStat(s.axes.position, likertItems, a.answers, q);
  const conflicted = new Set([...drivers.conflicts, ...process.conflicts].map((c) => c.key));

  const fc = forcedSection(q);
  const picks: Record<string, number> = Object.fromEntries(s.axes.drivers.map((k) => [k, 0]));
  for (const item of fc?.items ?? []) {
    const chosen = item.options.find((o) => o.id === a.forcedChoice[item.id]);
    if (chosen) picks[chosen.key] = (picks[chosen.key] ?? 0) + 1;
  }

  const driverDominant = dominantOf(drivers, q, flat);
  const driverConfidence: Record<string, Confidence> = {};
  for (const k of driverDominant) {
    driverConfidence[k] = conflicted.has(k) ? "low" : (picks[k] ?? 0) >= s.forcedChoice.agreeMinPicks ? "high" : "medium";
  }

  const processDominant = dominantOf(process, q, flat);
  const processConfidence: Record<string, Confidence> = {};
  for (const k of processDominant) {
    if (conflicted.has(k)) { processConfidence[k] = "low"; continue; }
    const matched = Object.entries(s.driverProcessCorrespondence).some(([d, p]) => p === k && driverDominant.includes(d));
    processConfidence[k] = matched ? STEP_UP.medium : "medium";
  }

  const answered = rawValues.length + Object.keys(a.forcedChoice).length;
  const self = position.raw.self ?? null;
  const other = position.raw.other ?? null;
  return {
    questionnaire: q.id,
    revision: q.revision,
    answered,
    drivers: {
      scores: roundScores(drivers.raw, digits),
      personMean: drivers.personMean === null ? null : round(drivers.personMean, digits),
      dominant: driverDominant,
      forcedChoice: picks,
      confidence: driverConfidence,
    },
    process: {
      scores: roundScores(process.raw, digits),
      personMean: process.personMean === null ? null : round(process.personMean, digits),
      dominant: processDominant,
      confidence: processConfidence,
    },
    position: {
      self: self === null ? null : round(self, digits),
      other: other === null ? null : round(other, digits),
      selfBand: band(self, q),
      otherBand: band(other, q),
    },
    responseStyle: {
      rawMean: rawMean === null ? null : round(rawMean, digits),
      rawSd: rawSd === null ? null : round(rawSd, digits),
      oneSided,
      flat,
      reverseConflicts: [...drivers.conflicts, ...process.conflicts, ...position.conflicts],
    },
  };
}

function main(): void {
  const [answersPath, questionnairePath] = process.argv.slice(2);
  if (!answersPath) throw new InputError("usage: score-life-script <answers.json> [questionnaire.json]");
  const q = loadQuestionnaire(questionnairePath ?? DEFAULT_QUESTIONNAIRE);
  const result = scoreLifeScript(q, parseAnswers(readJsonFile(answersPath), q));
  process.stdout.write(JSON.stringify({ lifeScript: result }, null, 2) + "\n");
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  try { main(); } catch (e) { failCli(e); }
}
