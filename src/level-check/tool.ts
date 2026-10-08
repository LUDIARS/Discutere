/** One level comparison plus pure conditional envelope arithmetic; no mechanics-use-case dependency. */
import { invokeDiagnostic, type DiagnosticDependencies } from "../design-diagnostic/invoke.js";
import { LEVEL_CHECK_LIMITS as L, LevelCheckError, type LevelCheckRequest, type LevelCheckResult } from "./contracts.js";
import { validateLevelCheckRequest } from "./request.js";
import { calculatePlayEnvelopes } from "./play-envelope.js";
import { composePlayFindings } from "./play-findings.js";
import { buildLevelCheckPrompt } from "./prompt.js";
import { parseLevelCheckResponse } from "./response.js";
export type LevelCheckDependencies = DiagnosticDependencies;
export async function checkLevelConsistency(input: LevelCheckRequest, deps: LevelCheckDependencies): Promise<LevelCheckResult> {
  const request = validateLevelCheckRequest(input);
  const envelopes = request.playModel ? calculatePlayEnvelopes(request.playModel) : [];
  const textual = parseLevelCheckResponse(await invokeDiagnostic(buildLevelCheckPrompt(request), deps), request);
  const numeric = composePlayFindings(request, envelopes);
  const result: LevelCheckResult = { logic_differences: [...textual.logic_differences, ...numeric.differences], design_gap: [...textual.design_gap, ...numeric.gaps] };
  if (!(request.references ?? []).some((r) => r.evidenceKind === "observation")) {
    result.design_gap.push({
      kind: "missing_evidence", target: "actual_level_experience",
      left: { text: "場面資料と条件付き模型の比較。", evidence: [{ referenceId: "level", quote: request.levelText.trim().slice(0, L.shortTextChars) }] },
      right: { text: "実プレイの観測資料は未提示。", evidence: [] },
      status: "unknown", causeDomain: "unknown", conditions: ["入力された場面・技能・配置・適用ルールの前提に限定。"],
      unknowns: ["実プレイの到達幅や体験は未確認。数値幅だけでは原因の所在を特定できない。"],
    });
  }
  if (Buffer.byteLength(JSON.stringify(result), "utf8") > L.resultBytes) throw new LevelCheckError("result_too_large", "Diagnostic result exceeds byte limit");
  return result;
}
