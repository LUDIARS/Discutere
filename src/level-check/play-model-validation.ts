/** Validate explicit joint outcomes and scenario boundaries before numeric calculation. */
import { LEVEL_CHECK_LIMITS as L, type PlayEvent, type PlayModel, type PlayOutcome, type PlayScenario } from "./contracts.js";
import { array, choice, fail, finite, id, object, string, unique } from "../design-diagnostic/validation.js";

const C = "invalid_input" as const;
export const PROBABILITY_TOLERANCE = 1e-9;
function ids(value: unknown, path: string, max: number, available: Set<string>, min = 0): string[] {
  const values = array(value, path, max, C, min).map((v, i) => id(v, `${path}[${i}]`, C));
  unique(values, path, C);
  if (values.some((v) => !available.has(v))) fail(C, path, "unknown reference ID");
  return values;
}
function event(value: unknown, i: number, references: Set<string>): PlayEvent {
  const path = `playModel.events[${i}]`;
  const e = object(value, path, ["id", "description", "determinant", "evidenceRefs"], C);
  return {
    id: id(e.id, `${path}.id`, C),
    description: string(e.description, `${path}.description`, L.shortTextChars, C),
    determinant: choice(e.determinant, ["random", "player_skill", "mixed", "deterministic", "unknown"], `${path}.determinant`, C),
    evidenceRefs: ids(e.evidenceRefs, `${path}.evidenceRefs`, L.references + 2, references, 1),
  };
}
function outcome(value: unknown, path: string, events: Set<string>, unit: string): PlayOutcome {
  const o = object(value, path, ["id", "eventIds", "value", "unit", "probability"], C);
  const outcomeUnit = string(o.unit, `${path}.unit`, 80, C);
  if (outcomeUnit !== unit) fail(C, `${path}.unit`, "does not match metric unit");
  const probability = o.probability === undefined ? undefined : finite(o.probability, `${path}.probability`, C);
  if (probability !== undefined && (probability < 0 || probability > 1)) fail(C, `${path}.probability`, "outside 0..1");
  return {
    id: id(o.id, `${path}.id`, C),
    eventIds: ids(o.eventIds, `${path}.eventIds`, L.events, events),
    value: finite(o.value, `${path}.value`, C),
    unit: outcomeUnit,
    ...(probability === undefined ? {} : { probability }),
  };
}
function scenario(value: unknown, i: number, events: Set<string>, unit: string): PlayScenario {
  const path = `playModel.scenarios[${i}]`;
  const s = object(value, path, ["id", "description", "skillBand", "assumptions", "predictedBaseline", "exhaustive", "outcomes"], C);
  const a = object(s.assumptions, `${path}.assumptions`, ["ruleset", "placement", "information", "initialState", "timeLimitSeconds", "trialCount", "other"], C);
  const assumptions: PlayScenario["assumptions"] = {};
  for (const key of ["ruleset", "placement", "information", "initialState"] as const) {
    if (a[key] !== undefined) assumptions[key] = string(a[key], `${path}.assumptions.${key}`, L.shortTextChars - 32, C);
  }
  if (a.timeLimitSeconds !== undefined) {
    const n = finite(a.timeLimitSeconds, `${path}.assumptions.timeLimitSeconds`, C);
    if (n <= 0) fail(C, path, "time limit must be positive");
    assumptions.timeLimitSeconds = n;
  }
  if (a.trialCount !== undefined) {
    const n = finite(a.trialCount, `${path}.assumptions.trialCount`, C);
    if (!Number.isSafeInteger(n) || n <= 0) fail(C, path, "trial count must be positive safe integer");
    assumptions.trialCount = n;
  }
  // Eight fixed condition descriptions may be added by the result composer.
  if (a.other !== undefined) assumptions.other = array(a.other, `${path}.assumptions.other`, L.conditions - 8, C).map((v, j) => string(v, `${path}.assumptions.other[${j}]`, L.shortTextChars, C));
  let predictedBaseline: PlayScenario["predictedBaseline"];
  if (s.predictedBaseline !== undefined) {
    const b = object(s.predictedBaseline, `${path}.predictedBaseline`, ["value", "unit"], C);
    const baselineUnit = string(b.unit, `${path}.predictedBaseline.unit`, 80, C);
    if (baselineUnit !== unit) fail(C, path, "baseline unit does not match metric");
    predictedBaseline = { value: finite(b.value, `${path}.predictedBaseline.value`, C), unit: baselineUnit };
  }
  if (typeof s.exhaustive !== "boolean") fail(C, `${path}.exhaustive`, "expected boolean");
  const outcomes = array(s.outcomes, `${path}.outcomes`, L.outcomesPerScenario, C, 1).map((v, j) => outcome(v, `${path}.outcomes[${j}]`, events, unit));
  unique(outcomes.map((o) => o.id), `${path}.outcomes`, C);
  unique(outcomes.map((o) => [...o.eventIds].sort().join("\u0000")), `${path}.outcomes joint combinations`, C);
  const probabilities = outcomes.flatMap((o) => o.probability === undefined ? [] : [o.probability]);
  const total = probabilities.reduce((sum, n) => sum + n, 0);
  if (total > 1 + PROBABILITY_TOLERANCE) fail(C, path, "supplied probabilities exceed 1");
  if (s.exhaustive && probabilities.length === outcomes.length && Math.abs(total - 1) > PROBABILITY_TOLERANCE) fail(C, path, "exhaustive joint probabilities must sum to 1");
  return {
    id: id(s.id, `${path}.id`, C),
    description: string(s.description, `${path}.description`, L.shortTextChars, C),
    ...(s.skillBand === undefined ? {} : { skillBand: string(s.skillBand, `${path}.skillBand`, L.shortTextChars - 32, C) }),
    assumptions, ...(predictedBaseline === undefined ? {} : { predictedBaseline }),
    exhaustive: s.exhaustive, outcomes,
  };
}
export function validatePlayModel(value: unknown, references: Set<string>): PlayModel {
  const model = object(value, "playModel", ["metric", "events", "scenarios"], C);
  const metric = object(model.metric, "playModel.metric", ["id", "name", "unit", "direction"], C);
  const unit = string(metric.unit, "playModel.metric.unit", 80, C);
  const events = array(model.events, "playModel.events", L.events, C).map((v, i) => event(v, i, references));
  unique(events.map((e) => e.id), "playModel.events", C);
  const scenarios = array(model.scenarios, "playModel.scenarios", L.scenarios, C, 1).map((v, i) => scenario(v, i, new Set(events.map((e) => e.id)), unit));
  unique(scenarios.map((s) => s.id), "playModel.scenarios", C);
  if (scenarios.reduce((sum, s) => sum + s.outcomes.length, 0) > L.totalOutcomes) fail(C, "playModel", `exceeds ${L.totalOutcomes} outcomes`);
  return {
    metric: { id: id(metric.id, "playModel.metric.id", C), name: string(metric.name, "playModel.metric.name", L.shortTextChars, C), unit, direction: choice(metric.direction, ["higher", "lower"], "playModel.metric.direction", C) },
    events, scenarios,
  };
}
