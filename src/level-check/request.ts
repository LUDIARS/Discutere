/** Bounded level documents and optional explicit joint-outcome model. */
import type { DiagnosticReference } from "../design-diagnostic/contracts.js";
import { object } from "../design-diagnostic/validation.js";
import { suppliedDocumentMap, validateDocuments } from "../design-diagnostic/documents.js";
import type { LevelCheckRequest } from "./contracts.js";
import { validatePlayModel } from "./play-model-validation.js";
export function validateLevelCheckRequest(value: unknown): LevelCheckRequest {
  const r = object(value, "request", ["levelText", "baselineText", "references", "playModel"], "invalid_input");
  const { primaryText, ...rest } = validateDocuments(r.levelText, r.baselineText, r.references, "levelText");
  const docs = suppliedDocumentMap("level", primaryText, rest.baselineText, rest.references);
  const playModel = r.playModel === undefined ? undefined : validatePlayModel(r.playModel, new Set(docs.keys()));
  return { levelText: primaryText, ...rest, ...(playModel === undefined ? {} : { playModel }) };
}
export function suppliedLevelDocuments(request: LevelCheckRequest): Map<string, DiagnosticReference> {
  const docs = suppliedDocumentMap("level", request.levelText, request.baselineText, request.references ?? []);
  if (request.playModel !== undefined) docs.set("playModel", { id: "playModel", source: "supplied conditional model (not observed play)", text: JSON.stringify(request.playModel), evidenceKind: "model_assumption" });
  return docs;
}
