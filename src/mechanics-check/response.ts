/** Feature-specific kind whitelist over shared citation/comparison validation. */
import { MECHANICS_DIFFERENCE_KINDS, type MechanicsCheckRequest, type MechanicsCheckResult } from "./contracts.js";
import { suppliedDocuments } from "./request.js";
import { parseDiagnosticResponse } from "../design-diagnostic/response.js";
export function parseMechanicsCheckResponse(text: string, request: MechanicsCheckRequest): MechanicsCheckResult {
  return parseDiagnosticResponse(text, suppliedDocuments(request), MECHANICS_DIFFERENCE_KINDS);
}
