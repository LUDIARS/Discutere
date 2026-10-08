/** Rule/resource/MDA diagnostic contracts; numeric scenario models belong to level-check. */
import { DIAGNOSTIC_LIMITS } from "../design-diagnostic/contracts.js";
import type { DiagnosticReference, DiagnosticResult, TextDifference } from "../design-diagnostic/contracts.js";
export { DiagnosticError as MechanicsCheckError } from "../design-diagnostic/contracts.js";
export type { DiagnosticErrorCode as MechanicsCheckErrorCode, DiagnosticReference as MechanicsReference, DiagnosticFinding as MechanicsFinding, EvidenceKind, EvidenceCitation, ComparedStatement, DesignGap, ConsistencyStatus, CauseDomain } from "../design-diagnostic/contracts.js";
export const MECHANICS_CHECK_LIMITS = DIAGNOSTIC_LIMITS;
export const MECHANICS_DIFFERENCE_KINDS = ["mda_causality", "resource_flow", "rule_dependency", "revision"] as const;
export type TextDifferenceKind = typeof MECHANICS_DIFFERENCE_KINDS[number];
export type TextLogicDifference = TextDifference<TextDifferenceKind>;
export interface MechanicsCheckRequest { specText: string; baselineText?: string; references?: DiagnosticReference[] }
export type MechanicsCheckResult = DiagnosticResult<TextDifferenceKind>;
