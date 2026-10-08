/** One rule/resource/MDA comparison; no level computation or discussion dependency. */
import { MECHANICS_CHECK_LIMITS as L, MechanicsCheckError, type MechanicsCheckRequest, type MechanicsCheckResult } from "./contracts.js";
import { validateMechanicsCheckRequest } from "./request.js";
import { buildMechanicsCheckPrompt } from "./prompt.js";
import { parseMechanicsCheckResponse } from "./response.js";
import { invokeDiagnostic, type DiagnosticDependencies } from "../design-diagnostic/invoke.js";
export type MechanicsCheckDependencies = DiagnosticDependencies;
export async function checkMechanicsConsistency(input: MechanicsCheckRequest, deps: MechanicsCheckDependencies): Promise<MechanicsCheckResult> {
  const request = validateMechanicsCheckRequest(input);
  const result = parseMechanicsCheckResponse(await invokeDiagnostic(buildMechanicsCheckPrompt(request), deps), request);
  if (!(request.references ?? []).some((r) => r.evidenceKind === "observation")) {
    result.design_gap.push({
      kind: "missing_evidence", target: "actual_play_experience",
      left: { text: "入力仕様に対する意図・因果の比較。", evidence: [{ referenceId: "spec", quote: request.specText.trim().slice(0, L.shortTextChars) }] },
      right: { text: "実プレイの観測資料は未提示。", evidence: [] },
      status: "unknown", causeDomain: "unknown", conditions: ["ルール・資源・MDAの条件付き論理比較。"],
      unknowns: ["実際の体験や面白さの証明は unknown。模型仮定・原因仮説を観測に置き換えない。"],
    });
  }
  if (Buffer.byteLength(JSON.stringify(result), "utf8") > L.resultBytes) throw new MechanicsCheckError("result_too_large", "Diagnostic result exceeds byte limit");
  return result;
}
