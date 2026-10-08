/** Strict mechanics documents; reject numeric models rather than silently forwarding them. */
import { MechanicsCheckError, type MechanicsCheckRequest, type MechanicsReference } from "./contracts.js";
import { object } from "../design-diagnostic/validation.js";
import { suppliedDocumentMap, validateDocuments } from "../design-diagnostic/documents.js";
export function validateMechanicsCheckRequest(value: unknown): MechanicsCheckRequest {
  if (value !== null && typeof value === "object" && Object.prototype.hasOwnProperty.call(value, "playModel")) {
    throw new MechanicsCheckError("use_level_check", "playModel belongs to /level-check");
  }
  const r = object(value, "request", ["specText", "baselineText", "references"], "invalid_input");
  const { primaryText, ...rest } = validateDocuments(r.specText, r.baselineText, r.references, "specText");
  return { specText: primaryText, ...rest };
}
export function suppliedDocuments(request: MechanicsCheckRequest): Map<string, MechanicsReference> {
  return suppliedDocumentMap("spec", request.specText, request.baselineText, request.references ?? []);
}
