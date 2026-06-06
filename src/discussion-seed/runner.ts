/**
 * Headless 議論ランナー (#64)。
 *
 * シード済 (seedHeadlessDiscussion) の gap を、 facilitator 機構で収束まで回す。
 * 進行役の idle 判定 (idleGapMs) を 0 にして tick ごとに介入させ、 拡張→止揚判定→
 * 収束を最短で進める (= 「裏で何度か回す」 を 1 プロセスで完結)。
 *
 * 収束すると facilitator が【収束】まとめを投稿し gap を closed にする (= 議論を締める)。
 */

import { createDiscatierContextProvider } from "../discatier-engine-adapter/index.js";
import { createFacilitator } from "../persona-engine/facilitator/index.js";
import type { PersonasRepo } from "../persona-engine/db/personas-repo.js";
import type { LLMClient } from "../persona-engine/index.js";
import type { createCore } from "../core/index.js";

type Core = ReturnType<typeof createCore>;

export interface RunHeadlessOptions {
  /** 収束しなくても打ち切る最大 tick 数 (既定 40) */
  maxRounds?: number;
  /** 止揚がこの数たまったら収束 (既定 2 — headless は短めに締める) */
  aufhebungTarget?: number;
  /** persona がこの数を超えたら強制収束 (既定 8) */
  maxPersonas?: number;
  /** LLM model 上書き */
  model?: string;
  /** 進捗ログ (既定 no-op) */
  onRound?: (round: number, info: { utterances: number; converged: boolean }) => void;
}

export interface RunHeadlessResult {
  gapId: string;
  sessionId: string;
  converged: boolean;
  rounds: number;
  utteranceCount: number;
  /** 収束まとめ (【収束】発言の本文)。 未収束なら null。 */
  conclusion: string | null;
}

export async function runHeadlessDiscussion(args: {
  core: Core;
  llm: LLMClient;
  personas: PersonasRepo;
  workspaceId: string;
  gapId: string;
  sessionId: string;
  options?: RunHeadlessOptions;
}): Promise<RunHeadlessResult> {
  const { core, llm, personas, workspaceId, gapId, sessionId } = args;
  const maxRounds = args.options?.maxRounds ?? 40;

  const contextProvider = createDiscatierContextProvider(core, {});
  const noopLogger = {
    debug: () => {},
    info: () => {},
    warn: () => {},
    error: () => {},
  };

  const facilitator = createFacilitator({
    core,
    llm,
    contextProvider,
    personas,
    workspaceId,
    logger: noopLogger,
    options: {
      idleGapMs: 0, // tick ごとに必ず介入させる (待ち時間無しで回す)
      aufhebungTarget: args.options?.aufhebungTarget ?? 2,
      maxPersonas: args.options?.maxPersonas ?? 8,
      model: args.options?.model,
    },
  });

  let rounds = 0;
  let converged = false;
  for (let i = 0; i < maxRounds; i++) {
    rounds = i + 1;
    await facilitator.tickOnce();
    converged = isGapConverged(core, gapId);
    args.options?.onRound?.(rounds, {
      utterances: countUtterances(core, sessionId),
      converged,
    });
    if (converged) break;
  }

  return {
    gapId,
    sessionId,
    converged,
    rounds,
    utteranceCount: countUtterances(core, sessionId),
    conclusion: latestConclusion(core, sessionId),
  };
}

function isGapConverged(core: Core, gapId: string): boolean {
  const row = core.client.raw
    .prepare("SELECT status FROM design_gaps WHERE id = ?")
    .get(gapId) as { status: string | null } | undefined;
  return !!row?.status && ["closed", "converged"].includes(row.status);
}

function countUtterances(core: Core, sessionId: string): number {
  const row = core.client.raw
    .prepare("SELECT COUNT(*) AS n FROM utterances WHERE session_id = ?")
    .get(sessionId) as { n: number };
  return row.n;
}

function latestConclusion(core: Core, sessionId: string): string | null {
  const row = core.client.raw
    .prepare(
      "SELECT raw_content FROM utterances WHERE session_id = ? AND raw_content LIKE '【収束】%' ORDER BY posted_at DESC LIMIT 1"
    )
    .get(sessionId) as { raw_content: string } | undefined;
  return row?.raw_content ?? null;
}
