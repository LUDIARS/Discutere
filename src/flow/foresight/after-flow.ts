/**
 * 議論/改善フローの結果に未来予測アドバイザリーを続ける (dispatch の後段)。
 *
 * オプショナル: 観点タグ「未来予測」を付けた議論だけで走る (neco 2026-10-18)。
 * config.flow.foresight.enabled=false なら、タグがあっても走らない (機能ごと止める)。
 * エンジン (rounds / dialectic) の結果 FlowDirectorResult だけを入力にするので、
 * どちらのエンジンにも同じ経路で付く。結論は変えない参考扱い。
 * フェーズ内の失敗は warn して本議論の結果をそのまま返す (議論を壊さない)。
 *
 * 進行状態 (state.ts) を sessionId ごとに記録し、Web の status が本議論の完了と
 * 未来予測の完了を分けて返せるようにする。
 */

import type { LLMClient } from "../../persona-engine/llm/client.js";
import { getConfig } from "../../config.js";
import { persistUtterance, type FlowDirectorResult, type FlowUtteranceRecord } from "../director.js";
import { getPaperBodyBySession, updatePaperBody, type ContextVoice } from "../discussion-paper.js";
import { wantsForesight, type FlowTag } from "../tags.js";
import { runForesightPhase } from "./phase.js";
import { stripForesight } from "./markdown.js";
import { CONTEXT_VOICE_LIMIT, type ForesightContext } from "./prompts.js";
import { setForesightStatus } from "./state.js";

/** prompt に載せる外部の声 1 件の上限 (字)。 */
const VOICE_CHARS = 200;

export interface ForesightAfterFlowDeps {
  llm: LLMClient;
  onUtterance?: (u: FlowUtteranceRecord) => void | Promise<void>;
  /** 本議論と同じ外部の声の検索 (無ければ声は渡さない)。 */
  listExternalVoices?: (terms: string[], limit: number) => ContextVoice[];
  log?: (msg: string) => void;
  warn?: (msg: string) => void;
}

/** このタグ集合で未来予測を行うか (機能の有効化 + タグ)。 */
export function foresightRequested(tags: readonly FlowTag[]): boolean {
  return getConfig().flow.foresight.enabled && wantsForesight(tags);
}

/**
 * 本議論の実行を包み、未来予測を続ける。sessionId が先に分かっている (Web) なら、本議論の実行中から
 * 「待機中」を記録して、結論が出た直後に完了と誤表示されないようにする。
 */
export async function runWithForesight(
  theme: string,
  tags: readonly FlowTag[],
  flow: string,
  run: () => Promise<FlowDirectorResult>,
  deps: ForesightAfterFlowDeps & { sessionId?: string }
): Promise<FlowDirectorResult> {
  const requested = foresightRequested(tags);
  if (requested && deps.sessionId) setForesightStatus(deps.sessionId, "pending");
  let result: FlowDirectorResult;
  try {
    result = await run();
  } catch (e) {
    if (requested && deps.sessionId) setForesightStatus(deps.sessionId, "failed", "本議論が例外で止まった");
    throw e;
  }
  return continueWithForesight(theme, tags, flow, result, deps);
}

/** 本議論の文脈 (ペーパー本文 + 外部の声)。取れなければ空で続ける。 */
function buildContext(
  theme: string,
  sessionId: string,
  deps: ForesightAfterFlowDeps,
  warn: (m: string) => void
): ForesightContext {
  let paperMd = "";
  try {
    paperMd = stripForesight(getPaperBodyBySession(sessionId) ?? "");
  } catch (e) {
    warn(`未来予測: ペーパー本文を読めなかった (${(e as Error).message})`);
  }
  let voices: ForesightContext["voices"] = [];
  if (deps.listExternalVoices) {
    try {
      voices = deps.listExternalVoices([theme], CONTEXT_VOICE_LIMIT).map((v) => ({
        content: v.content.length > VOICE_CHARS ? `${v.content.slice(0, VOICE_CHARS)}…` : v.content,
        source: v.source,
      }));
    } catch (e) {
      warn(`未来予測: 外部の声を引けなかった (${(e as Error).message})`);
    }
  }
  return { paperMd, voices };
}

export async function continueWithForesight(
  theme: string,
  tags: readonly FlowTag[],
  flow: string,
  result: FlowDirectorResult,
  deps: ForesightAfterFlowDeps
): Promise<FlowDirectorResult> {
  if (!foresightRequested(tags)) return result;
  const cfg = getConfig().flow.foresight;
  const log = deps.log ?? ((m: string) => console.log(`[flow/foresight] ${m}`));
  const warn = deps.warn ?? ((m: string) => console.warn(`[flow/foresight/warn] ${m}`));
  const sessionId = result.sessionId;

  // 結論の無い議論から未来は予測しない (土台が無いまま一手を作らせない。2026-10-18 Astra 評価)。
  if (!result.concluded || !result.conclusion.trim()) {
    log("未来予測: 本議論が収束していないため行わない");
    setForesightStatus(sessionId, "skipped", "本議論が収束していない");
    return result;
  }

  setForesightStatus(sessionId, "running");
  try {
    const phase = await runForesightPhase({
      theme,
      sessionId,
      paperId: result.paperId,
      flow,
      llm: deps.llm,
      utterances: result.utterances,
      conclusion: result.conclusion,
      context: buildContext(theme, sessionId, deps, warn),
      turns: cfg.turns,
      model: cfg.model || undefined,
      forecastModel: cfg.forecastModel || cfg.model || undefined,
      commit: async (record) => {
        persistUtterance(record);
        if (deps.onUtterance) await deps.onUtterance(record);
      },
      paperBody: {
        get: () => getPaperBodyBySession(sessionId),
        set: (bodyMd) => updatePaperBody(result.paperId, bodyMd),
      },
      log,
      warn,
    });
    if (!phase) {
      setForesightStatus(sessionId, "skipped", "改革派と保守派に分けられる話者がいない");
      return result;
    }
    setForesightStatus(
      sessionId,
      phase.status,
      phase.status === "done" ? undefined : `失敗した段 ${phase.failures} 件`
    );
    return {
      ...result,
      utterances: [...result.utterances, ...phase.utterances],
      foresight: { proposal: phase.proposal, forecast: phase.forecast },
    };
  } catch (e) {
    warn(`未来予測を中断 (本議論の結果は維持): ${(e as Error).message}`);
    setForesightStatus(sessionId, "failed", `例外: ${(e as Error).message}`);
    return result;
  }
}
