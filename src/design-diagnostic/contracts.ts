/** Shared documents and structured comparisons, without feature-specific computation. */
export const DIAGNOSTIC_LIMITS = {
  documentChars: 20_000, referenceChars: 8_000, totalTextChars: 64_000,
  references: 16, findingsPerKind: 32, shortTextChars: 512, statementChars: 2_048,
  conditions: 16, responseBytes: 131_072, resultBytes: 524_288,
  discordInputChars: 6_000, timeoutMs: 60_000,
} as const;
export type EvidenceKind = "specification" | "static_implementation" | "observation" | "model_assumption" | "causal_hypothesis";
export type CauseDomain = "mechanics" | "level" | "both" | "unknown";
export type ConsistencyStatus = "consistent" | "inconsistent" | "unknown";
export interface DiagnosticReference {
  id: string;
  source: string;
  revision?: string;
  text: string;
  evidenceKind: EvidenceKind;
  /** Supplied content relevance, not proof of a causal attribution. Omitted = unknown. */
  domain?: "mechanics" | "level";
}
export interface EvidenceCitation { referenceId: string; quote: string }
export interface ComparedStatement { text: string; evidence: EvidenceCitation[] }
export interface DiagnosticFinding {
  target: string;
  left: ComparedStatement;
  right: ComparedStatement;
  status: ConsistencyStatus;
  /** Conditional cause hypothesis, never inferred merely from a numeric range. */
  causeDomain: CauseDomain;
  conditions: string[];
  unknowns: string[];
}
export interface TextDifference<K extends string> extends DiagnosticFinding { kind: K }
export type DesignGap = DiagnosticFinding & (
  | { kind: "missing_evidence" | "missing_condition"; status: "unknown" }
  | { kind: "contradiction"; status: "inconsistent" }
);
export interface DiagnosticResult<K extends string> { logic_differences: TextDifference<K>[]; design_gap: DesignGap[] }
export type DiagnosticErrorCode = "invalid_input" | "use_level_check" | "llm_unavailable" | "llm_failed"
  | "llm_timeout" | "invalid_response" | "result_too_large";
export class DiagnosticError extends Error {
  constructor(public readonly code: DiagnosticErrorCode, message: string) {
    super(message);
    this.name = "DiagnosticError";
  }
}
