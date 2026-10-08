/** Bounded, transport-independent contracts for the single-shot diagnostic. */
export const MECHANICS_CHECK_LIMITS = {
  documentChars: 20_000,
  referenceChars: 8_000,
  totalTextChars: 64_000,
  references: 16,
  events: 32,
  scenarios: 16,
  outcomesPerScenario: 128,
  totalOutcomes: 256,
  findingsPerKind: 32,
  shortTextChars: 512,
  statementChars: 2_048,
  conditions: 16,
  responseBytes: 131_072,
  resultBytes: 524_288,
  discordInputChars: 6_000,
  timeoutMs: 60_000,
} as const;

export type EvidenceKind = "specification" | "static_implementation" | "observation"
  | "model_assumption" | "causal_hypothesis";
export type Determinant = "random" | "player_skill" | "mixed" | "deterministic" | "unknown";
export type ConsistencyStatus = "consistent" | "inconsistent" | "unknown";

export interface MechanicsReference {
  id: string;
  source: string;
  revision?: string;
  text: string;
  evidenceKind: EvidenceKind;
}

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

export interface MechanicsCheckRequest {
  specText: string;
  baselineText?: string;
  references?: MechanicsReference[];
  playModel?: PlayModel;
}

export interface EvidenceCitation { referenceId: string; quote: string }
export interface ComparedStatement { text: string; evidence: EvidenceCitation[] }
export interface MechanicsFinding {
  target: string;
  left: ComparedStatement;
  right: ComparedStatement;
  status: ConsistencyStatus;
  conditions: string[];
  unknowns: string[];
}
export type TextDifferenceKind = "mda_causality" | "resource_flow" | "rule_dependency"
  | "revision" | "qualitative_play";
export interface TextLogicDifference extends MechanicsFinding { kind: TextDifferenceKind }
export type DesignGap = MechanicsFinding & (
  | { kind: "missing_evidence" | "missing_condition"; status: "unknown" }
  | { kind: "contradiction"; status: "inconsistent" }
);

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
export interface PlayEnvelopeDifference extends MechanicsFinding {
  kind: "play_envelope";
  /** Computed on the server, never accepted from the LLM. */
  envelope: PlayEnvelope;
}
export interface MechanicsCheckResult {
  logic_differences: (TextLogicDifference | PlayEnvelopeDifference)[];
  design_gap: DesignGap[];
}

export type MechanicsCheckErrorCode = "invalid_input" | "llm_unavailable" | "llm_failed"
  | "llm_timeout" | "invalid_response" | "result_too_large";
export class MechanicsCheckError extends Error {
  constructor(public readonly code: MechanicsCheckErrorCode, message: string) {
    super(message);
    this.name = "MechanicsCheckError";
  }
}
