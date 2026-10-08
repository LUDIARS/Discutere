/** Scenario/skill/placement contracts and explicit conditional play envelopes. */
import { DIAGNOSTIC_LIMITS } from "../design-diagnostic/contracts.js";
import type { DiagnosticReference, DiagnosticFinding, DesignGap, TextDifference } from "../design-diagnostic/contracts.js";
export { DiagnosticError as LevelCheckError } from "../design-diagnostic/contracts.js";
export type { DiagnosticReference as LevelReference } from "../design-diagnostic/contracts.js";
export const LEVEL_CHECK_LIMITS = { ...DIAGNOSTIC_LIMITS, events: 32, scenarios: 16, outcomesPerScenario: 128, totalOutcomes: 256 } as const;
export type Determinant = "random" | "player_skill" | "mixed" | "deterministic" | "unknown";
export interface PlayEvent {
  id: string;
  description: string;
  determinant: Determinant;
  evidenceRefs: string[];
}

export interface PlayOutcome {
  id: string;
  /** Exact joint occurrence; unlisted events are absent. No independence assumption. */
  eventIds: string[];
  value: number;
  unit: string;
  probability?: number;
}

export interface PlayScenario {
  id: string;
  description: string;
  skillBand?: string;
  assumptions: {
    ruleset?: string;
    placement?: string;
    information?: string;
    initialState?: string;
    timeLimitSeconds?: number;
    trialCount?: number;
    other?: string[];
  };
  predictedBaseline?: { value: number; unit: string };
  exhaustive: boolean;
  outcomes: PlayOutcome[];
}

export interface PlayModel {
  metric: { id: string; name: string; unit: string; direction: "higher" | "lower" };
  events: PlayEvent[];
  scenarios: PlayScenario[];
}


export interface LevelCheckRequest { levelText: string; baselineText?: string; references?: DiagnosticReference[]; playModel?: PlayModel }
export const LEVEL_DIFFERENCE_KINDS = ["scenario_condition", "placement", "skill_condition", "predictability", "revision"] as const;
export type LevelDifferenceKind = typeof LEVEL_DIFFERENCE_KINDS[number];
export type LevelTextDifference = TextDifference<LevelDifferenceKind>;
export interface PlayEnvelope {
  scenarioId: string;
  metric: PlayModel["metric"];
  skillBand: string | null;
  determinant: Determinant;
  rangeScope: "enumerated_complete" | "enumerated_partial";
  min: number;
  max: number;
  predictedBaseline: number | null;
  upside: number | null;
  downside: number | null;
  expectedValue: number | null;
  variance: number | null;
  probabilityStatus: "complete" | "missing_or_partial" | "non_exhaustive";
}
export interface PlayEnvelopeDifference extends DiagnosticFinding {
  kind: "play_envelope";
  /** Computed on the server, never accepted from the LLM. */
  envelope: PlayEnvelope;
}

export interface LevelCheckResult { logic_differences: (LevelTextDifference | PlayEnvelopeDifference)[]; design_gap: DesignGap[] }
