/** Accept only bounded structured comparisons with citations to actual supplied text. */
import { DIAGNOSTIC_LIMITS as L, type ComparedStatement, type DesignGap, type DiagnosticFinding, type DiagnosticReference, type DiagnosticResult, type TextDifference } from "./contracts.js";
import { array, choice, fail, id, object, string } from "./validation.js";

const C = "invalid_response" as const;
function strings(value: unknown, path: string): string[] {
  return array(value, path, L.conditions, C).map((v, i) => string(v, `${path}[${i}]`, L.shortTextChars, C));
}
function statement(value: unknown, path: string, docs: Map<string, DiagnosticReference>): ComparedStatement {
  const s = object(value, path, ["text", "evidence"], C);
  const evidence = array(s.evidence, `${path}.evidence`, L.references, C).map((v, i) => {
    const p = `${path}.evidence[${i}]`;
    const cite = object(v, p, ["referenceId", "quote"], C);
    const referenceId = id(cite.referenceId, `${p}.referenceId`, C);
    const quote = string(cite.quote, `${p}.quote`, L.shortTextChars, C);
    const doc = docs.get(referenceId);
    if (!doc || !doc.text.includes(quote)) fail(C, p, "citation is not an exact extract of a supplied document");
    return { referenceId, quote };
  });
  return { text: string(s.text, `${path}.text`, L.statementChars, C), evidence };
}
function finding(value: unknown, path: string, docs: Map<string, DiagnosticReference>): { raw: Record<string, unknown>; finding: DiagnosticFinding } {
  const raw = object(value, path, ["kind", "target", "left", "right", "status", "causeDomain", "conditions", "unknowns"], C);
  const left = statement(raw.left, `${path}.left`, docs);
  const right = statement(raw.right, `${path}.right`, docs);
  let status = choice(raw.status, ["consistent", "inconsistent", "unknown"], `${path}.status`, C);
  const conditions = strings(raw.conditions, `${path}.conditions`);
  const unknowns = strings(raw.unknowns, `${path}.unknowns`);
  let causeDomain = choice(raw.causeDomain, ["mechanics", "level", "both", "unknown"], `${path}.causeDomain`, C);
  const grounded = (s: ComparedStatement): boolean => s.evidence.some((e) => docs.get(e.referenceId)?.evidenceKind !== "causal_hypothesis");
  if (status !== "unknown" && (!grounded(left) || !grounded(right))) {
    status = "unknown";
    if (unknowns.length >= L.conditions) fail(C, path, "no room to describe unsupported conclusion");
    unknowns.push("比較の片側に入力資料の根拠がない、または原因仮説だけなので判定は unknown。");
  }
  const citedDomains = new Set([...left.evidence, ...right.evidence].flatMap((e) => {
    const doc = docs.get(e.referenceId);
    return doc?.domain && doc.evidenceKind !== "causal_hypothesis" ? [doc.domain] : [];
  }));
  const requiredDomains = causeDomain === "both" ? ["mechanics", "level"] : [causeDomain];
  if (causeDomain !== "unknown" && (!conditions.length || !grounded(left) || !grounded(right) || requiredDomains.some((d) => !citedDomains.has(d as "mechanics" | "level")))) {
    causeDomain = "unknown";
    if (unknowns.length >= L.conditions) fail(C, path, "no room to describe unsupported cause classification");
    unknowns.push("原因の領域を分類する比較条件または領域別の資料根拠がないため causeDomain は unknown。");
  }
  if (status !== "unknown" && !conditions.length) fail(C, path, "known conclusion needs explicit conditions");
  if (status === "unknown" && !unknowns.length) fail(C, path, "unknown conclusion needs missing evidence or conditions");
  return { raw, finding: { target: string(raw.target, `${path}.target`, L.shortTextChars, C), left, right, status, causeDomain, conditions, unknowns } };
}
export function parseDiagnosticResponse<K extends string>(text: string, docs: Map<string, DiagnosticReference>, kinds: readonly K[]): DiagnosticResult<K> {
  if (typeof text !== "string" || Buffer.byteLength(text, "utf8") > L.responseBytes) fail(C, "response", "response exceeds byte limit");
  let value: unknown;
  try { value = JSON.parse(text); } catch { fail(C, "response", "expected JSON only"); }
  const result = object(value, "response", ["logic_differences", "design_gap"], C);
  const logic_differences = array(result.logic_differences, "logic_differences", L.findingsPerKind, C).map((v, i): TextDifference<K> => {
    const path = `logic_differences[${i}]`;
    const f = finding(v, path, docs);
    return { ...f.finding, kind: choice(f.raw.kind, kinds, `${path}.kind`, C) };
  });
  const design_gap = array(result.design_gap, "design_gap", L.findingsPerKind, C).map((v, i): DesignGap => {
    const path = `design_gap[${i}]`;
    const f = finding(v, path, docs);
    const kind = choice(f.raw.kind, ["missing_evidence", "missing_condition", "contradiction"], `${path}.kind`, C);
    if (kind === "contradiction" && f.finding.status === "inconsistent") return { ...f.finding, kind, status: "inconsistent" };
    if (f.finding.status !== "unknown") fail(C, path, "missing evidence/condition gaps must be unknown; contradictions must be inconsistent");
    return { ...f.finding, status: "unknown", kind: kind === "contradiction" ? "missing_evidence" : kind };
  });
  if (!logic_differences.length && !design_gap.length) fail(C, "response", "empty diagnostic is not a successful result");
  // A downgraded unsupported comparison must also expose the missing evidence as a gap.
  for (const difference of logic_differences) {
    if ((difference.status === "unknown" || difference.unknowns.some((u) => u.includes("causeDomain は unknown"))) && !design_gap.some((gap) => gap.target === difference.target)) {
      design_gap.push({ ...difference, kind: "missing_evidence", status: "unknown" });
    }
    if (difference.status === "inconsistent" && difference.kind !== "revision" && !design_gap.some((gap) => gap.target === difference.target && gap.status === "inconsistent")) {
      design_gap.push({ ...difference, kind: "contradiction", status: "inconsistent" });
    }
  }
  return { logic_differences, design_gap };
}
