/**
 * 議論/改善フローの結果に未来予測フェーズを続ける (dispatch の後段)。
 *
 * エンジン (rounds / dialectic) の結果 FlowDirectorResult だけを入力にするので、
 * どちらのエンジンにも同じ経路で付く。config.flow.foresight.enabled=false なら何もしない。
 * フェーズ内の失敗は warn して本議論の結果をそのまま返す (議論を壊さない)。
 */

import type { LLMClient } from "../../persona-engine/llm/client.js";
import { getConfig } from "../../config.js";
import { persistUtterance, type FlowDirectorResult, type FlowUtteranceRecord } from "../director.js";
import { getPaperBodyBySession, updatePaperBody } from "../discussion-paper.js";
import { runForesightPhase } from "./phase.js";

export interface ForesightAfterFlowDeps {
  llm: LLMClient;
  onUtterance?: (u: FlowUtteranceRecord) => void | Promise<void>;
  log?: (msg: string) => void;
  warn?: (msg: string) => void;
}

export async function continueWithForesight(
  theme: string,
  flow: string,
  result: FlowDirectorResult,
  deps: ForesightAfterFlowDeps
): Promise<FlowDirectorResult> {
  const cfg = getConfig().flow.foresight;
  if (!cfg.enabled) return result;
  const log = deps.log ?? ((m: string) => console.log(`[flow/foresight] ${m}`));
  const warn = deps.warn ?? ((m: string) => console.warn(`[flow/foresight/warn] ${m}`));

  try {
    const phase = await runForesightPhase({
      theme,
      sessionId: result.sessionId,
      paperId: result.paperId,
      flow,
      llm: deps.llm,
      utterances: result.utterances,
      conclusion: result.concluded ? result.conclusion : "",
      turns: cfg.turns,
      model: cfg.model || undefined,
      commit: async (record) => {
        persistUtterance(record);
        if (deps.onUtterance) await deps.onUtterance(record);
      },
      paperBody: {
        get: () => getPaperBodyBySession(result.sessionId),
        set: (bodyMd) => updatePaperBody(result.paperId, bodyMd),
      },
      log,
      warn,
    });
    if (!phase) return result;
    return {
      ...result,
      utterances: [...result.utterances, ...phase.utterances],
      foresight: { proposal: phase.proposal, forecast: phase.forecast },
    };
  } catch (e) {
    warn(`未来予測を中断 (本議論の結果は維持): ${(e as Error).message}`);
    return result;
  }
}
