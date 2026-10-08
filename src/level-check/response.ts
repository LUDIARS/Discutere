/** LLM supplies only level comparisons; play_envelope is never accepted here. */
import { parseDiagnosticResponse } from "../design-diagnostic/response.js";
import { LEVEL_DIFFERENCE_KINDS, type LevelCheckRequest, type LevelCheckResult } from "./contracts.js";
import { suppliedLevelDocuments } from "./request.js";
export function parseLevelCheckResponse(text: string, request: LevelCheckRequest): LevelCheckResult {
  return parseDiagnosticResponse(text, suppliedLevelDocuments(request), LEVEL_DIFFERENCE_KINDS);
}
