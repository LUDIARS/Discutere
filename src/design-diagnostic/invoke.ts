/** One conversation-only invocation with bounded timeout and safe error disclosure. */
import type { LLMClient, LLMResult } from "../persona-engine/llm/client.js";
import { DIAGNOSTIC_LIMITS as L, DiagnosticError } from "./contracts.js";
export interface DiagnosticDependencies { llm: LLMClient; model?: string }
export async function invokeDiagnostic(prompt: { system: string; prompt: string }, deps: DiagnosticDependencies): Promise<string> {
  if (!deps?.llm || typeof deps.llm.invoke !== "function") throw new DiagnosticError("llm_unavailable", "Diagnostic requires a configured LLM client");
  let timer: ReturnType<typeof setTimeout> | undefined;
  let response: LLMResult;
  try {
    response = await Promise.race([
      deps.llm.invoke({ ...prompt, model: deps.model, maxTokens: 8_000, timeoutMs: L.timeoutMs, conversationOnly: true }),
      new Promise<never>((_, reject) => { timer = setTimeout(() => reject(new DiagnosticError("llm_timeout", "Diagnostic timed out")), L.timeoutMs); }),
    ]);
  } catch (error) {
    if (error instanceof DiagnosticError) throw error;
    throw new DiagnosticError("llm_failed", "Diagnostic LLM invocation failed");
  } finally { if (timer !== undefined) clearTimeout(timer); }
  if (!response?.ok) throw new DiagnosticError("llm_failed", "Diagnostic LLM returned failure");
  return response.text;
}
