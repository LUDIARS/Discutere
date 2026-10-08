/** Reusable one-shot diagnostic. This module has no discussion, KG or learning dependency. */
import type { LLMClient, LLMResult } from "../persona-engine/llm/client.js";
import { MECHANICS_CHECK_LIMITS as L, MechanicsCheckError, type MechanicsCheckRequest, type MechanicsCheckResult } from "./contracts.js";
import { validateMechanicsCheckRequest } from "./request.js";
import { calculatePlayEnvelopes } from "./play-envelope.js";
import { composePlayFindings } from "./play-findings.js";
import { buildMechanicsCheckPrompt } from "./prompt.js";
import { parseMechanicsCheckResponse } from "./response.js";

export interface MechanicsCheckDependencies { llm: LLMClient; model?: string }

/** Exactly one invoke, explicit timeout/failure, no retry and no execution privileges. */
export async function checkMechanicsConsistency(input: MechanicsCheckRequest, deps: MechanicsCheckDependencies): Promise<MechanicsCheckResult> {
  const request = validateMechanicsCheckRequest(input);
  if (!deps?.llm || typeof deps.llm.invoke !== "function") throw new MechanicsCheckError("llm_unavailable", "Mechanics check requires a configured LLM client");
  const envelopes = request.playModel ? calculatePlayEnvelopes(request.playModel) : [];
  const { system, prompt } = buildMechanicsCheckPrompt(request);
  let timer: ReturnType<typeof setTimeout> | undefined;
  let response: LLMResult;
  try {
    response = await Promise.race([
      deps.llm.invoke({ system, prompt, model: deps.model, maxTokens: 8_000, timeoutMs: L.timeoutMs, conversationOnly: true }),
      new Promise<never>((_, reject) => {
        timer = setTimeout(() => reject(new MechanicsCheckError("llm_timeout", "Mechanics check timed out")), L.timeoutMs);
      }),
    ]);
  } catch (error) {
    if (error instanceof MechanicsCheckError) throw error;
    // Backend messages can contain credentials or supplied text; never expose them.
    throw new MechanicsCheckError("llm_failed", "Mechanics check LLM invocation failed");
  } finally {
    if (timer !== undefined) clearTimeout(timer);
  }
  if (!response.ok) throw new MechanicsCheckError("llm_failed", "Mechanics check LLM returned failure");
  const textual = parseMechanicsCheckResponse(response.text, request);
  const numeric = composePlayFindings(request, envelopes);
  const result: MechanicsCheckResult = { logic_differences: [...textual.logic_differences, ...numeric.differences], design_gap: [...textual.design_gap, ...numeric.gaps] };
  if (!(request.references ?? []).some((r) => r.evidenceKind === "observation")) {
    result.design_gap.push({
      kind: "missing_evidence", target: "actual_play_experience",
      left: { text: "入力仕様に対する意図・因果の比較。", evidence: [{ referenceId: "spec", quote: request.specText.trim().slice(0, L.shortTextChars) }] },
      right: { text: "実プレイの観測資料は未提示。", evidence: [] },
      status: "unknown", conditions: ["仕様や模型の論理整合性に限定した診断。"],
      unknowns: ["実際の体験や面白さの証明は unknown。模型仮定・原因仮説を観測に置き換えない。"],
    });
  }
  if (Buffer.byteLength(JSON.stringify(result), "utf8") > L.resultBytes) throw new MechanicsCheckError("result_too_large", "Mechanics check result exceeds byte limit");
  return result;
}
