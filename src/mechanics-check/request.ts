/** Validate and copy the supplied documents; do not resolve URLs or touch persisted state. */
import { MECHANICS_CHECK_LIMITS as L, type MechanicsCheckRequest, type MechanicsReference } from "./contracts.js";
import { array, choice, fail, id, object, string, unique } from "./validation.js";
import { validatePlayModel } from "./play-model-validation.js";

const C = "invalid_input" as const;
const RESERVED = ["spec", "baseline", "playModel"];
function reference(value: unknown, index: number): MechanicsReference {
  const path = `references[${index}]`;
  const r = object(value, path, ["id", "source", "revision", "text", "evidenceKind"], C);
  const referenceId = id(r.id, `${path}.id`, C);
  if (RESERVED.includes(referenceId)) fail(C, `${path}.id`, "reserved document ID");
  return {
    id: referenceId,
    source: string(r.source, `${path}.source`, L.shortTextChars, C),
    ...(r.revision === undefined ? {} : { revision: string(r.revision, `${path}.revision`, L.shortTextChars, C) }),
    text: string(r.text, `${path}.text`, L.referenceChars, C),
    evidenceKind: choice(r.evidenceKind, ["specification", "static_implementation", "observation", "model_assumption", "causal_hypothesis"], `${path}.evidenceKind`, C),
  };
}

export function validateMechanicsCheckRequest(value: unknown): MechanicsCheckRequest {
  const r = object(value, "request", ["specText", "baselineText", "references", "playModel"], C);
  const specText = string(r.specText, "specText", L.documentChars, C);
  const baselineText = r.baselineText === undefined ? undefined : string(r.baselineText, "baselineText", L.documentChars, C);
  const references = r.references === undefined ? [] : array(r.references, "references", L.references, C).map(reference);
  unique(references.map((ref) => ref.id), "references", C);
  if (specText.length + (baselineText?.length ?? 0) + references.reduce((sum, ref) => sum + ref.text.length, 0) > L.totalTextChars) {
    fail(C, "request", `document text exceeds ${L.totalTextChars} characters`);
  }
  const availableIds = new Set(["spec", ...(baselineText === undefined ? [] : ["baseline"]), ...references.map((ref) => ref.id)]);
  const playModel = r.playModel === undefined ? undefined : validatePlayModel(r.playModel, availableIds);
  return { specText, ...(baselineText === undefined ? {} : { baselineText }), references, ...(playModel === undefined ? {} : { playModel }) };
}

/** Reserved IDs exist only when their corresponding input is supplied. */
export function suppliedDocuments(request: MechanicsCheckRequest): Map<string, MechanicsReference> {
  const docs = new Map((request.references ?? []).map((ref) => [ref.id, ref]));
  docs.set("spec", { id: "spec", source: "supplied specText", text: request.specText, evidenceKind: "specification" });
  if (request.baselineText !== undefined) docs.set("baseline", { id: "baseline", source: "supplied baselineText", text: request.baselineText, evidenceKind: "specification" });
  if (request.playModel !== undefined) docs.set("playModel", { id: "playModel", source: "supplied playModel (conditional model, not observed play)", text: JSON.stringify(request.playModel), evidenceKind: "model_assumption" });
  return docs;
}
