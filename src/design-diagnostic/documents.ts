/** Validate supplied text and provenance; never fetch a referenced source. */
import { DIAGNOSTIC_LIMITS as L, type DiagnosticReference } from "./contracts.js";
import { array, choice, fail, id, object, string, unique } from "./validation.js";
const C = "invalid_input" as const;
const RESERVED = ["spec", "level", "baseline", "playModel"];
function reference(value: unknown, index: number): DiagnosticReference {
  const path = `references[${index}]`;
  const r = object(value, path, ["id", "source", "revision", "text", "evidenceKind", "domain"], C);
  const referenceId = id(r.id, `${path}.id`, C);
  if (RESERVED.includes(referenceId)) fail(C, `${path}.id`, "reserved document ID");
  return {
    id: referenceId, source: string(r.source, `${path}.source`, L.shortTextChars, C),
    ...(r.revision === undefined ? {} : { revision: string(r.revision, `${path}.revision`, L.shortTextChars, C) }),
    text: string(r.text, `${path}.text`, L.referenceChars, C),
    evidenceKind: choice(r.evidenceKind, ["specification", "static_implementation", "observation", "model_assumption", "causal_hypothesis"], `${path}.evidenceKind`, C),
    ...(r.domain === undefined ? {} : { domain: choice(r.domain, ["mechanics", "level"] as const, `${path}.domain`, C) }),
  };
}
export function validateDocuments(primary: unknown, baseline: unknown, rawReferences: unknown, primaryField: "specText" | "levelText"): { primaryText: string; baselineText?: string; references: DiagnosticReference[] } {
  const primaryText = string(primary, primaryField, L.documentChars, C);
  const baselineText = baseline === undefined ? undefined : string(baseline, "baselineText", L.documentChars, C);
  const references = rawReferences === undefined ? [] : array(rawReferences, "references", L.references, C).map(reference);
  unique(references.map((ref) => ref.id), "references", C);
  if (primaryText.length + (baselineText?.length ?? 0) + references.reduce((sum, ref) => sum + ref.text.length, 0) > L.totalTextChars) fail(C, "request", `document text exceeds ${L.totalTextChars} characters`);
  return { primaryText, ...(baselineText === undefined ? {} : { baselineText }), references };
}
export function suppliedDocumentMap(primaryId: "spec" | "level", text: string, baselineText: string | undefined, references: DiagnosticReference[]): Map<string, DiagnosticReference> {
  const docs = new Map(references.map((ref) => [ref.id, ref]));
  const domain = primaryId === "spec" ? "mechanics" : "level";
  docs.set(primaryId, { id: primaryId, source: `supplied ${primaryId} text`, text, evidenceKind: "specification", domain });
  if (baselineText !== undefined) docs.set("baseline", { id: "baseline", source: "supplied baselineText", text: baselineText, evidenceKind: "specification", domain });
  return docs;
}
