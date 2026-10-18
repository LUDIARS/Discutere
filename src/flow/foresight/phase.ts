/**
 * 未来予測フェーズ (spec/feature/flow/foresight.md) の進行。
 *
 * 本議論 (議論 / 改善、rounds / dialectic どちらのエンジンでも) の結論の後に、
 *   [F1] 進行役が破壊可能な一手 + 起きること + リスクを提案
 *   [F2] 改革派 (賛同) と 保守派 (否定) が交互に論じる
 *   [F3] 進行役が 3 / 5 / 10 年後を予測し、ビジネススキームなら利益率・確度・ノウハウ強度を分析
 * を 1 ラウンド分の発話として流す。発話は本議論と同じ flow_utterance に永続し onUtterance で通知する。
 * 提案と予測はペーパー本文末尾の「# 未来予測」節にも焼く。
 *
 * LLM 失敗は発話にエラーを出して続ける (OVERVIEW §10)。提案が出なければ以降を行わない。
 */

import { randomUUID } from "node:crypto";
import type { LLMClient } from "../../persona-engine/llm/client.js";
import type { FlowUtteranceRecord } from "../director.js";
import { withCostLog } from "../cost-logger.js";
import { buildForesightCast, buildCampTurnOrder, type ForesightCast } from "./camps.js";
import {
  buildProposalPrompt,
  buildCampTurnPrompt,
  buildForecastPrompt,
  type ForesightLine,
} from "./prompts.js";
import { appendForesightMarkdown } from "./markdown.js";

/** Discord の 1 投稿 (webhook は 1900 字で切る) に収める上限。 */
const POST_LIMIT = 1800;

export interface ForesightPhaseArgs {
  theme: string;
  sessionId: string;
  paperId: string;
  flow: string;
  llm: LLMClient;
  /** 本議論の発話 (話者と陣営の引き継ぎ元)。 */
  utterances: readonly FlowUtteranceRecord[];
  /** 本議論の結論 (結論なしなら空でよい)。 */
  conclusion: string;
  /** 改革派 / 保守派の発言回数。 */
  turns: number;
  /** 使うモデル (undefined なら LLM の既定)。 */
  model?: string;
  /** 発話の永続 + 通知 (本議論と同じ経路)。 */
  commit: (record: FlowUtteranceRecord) => Promise<void>;
  /** ペーパー本文の読み書き (「# 未来予測」節を焼く)。 */
  paperBody?: { get(): string | null; set(bodyMd: string): void };
  log: (msg: string) => void;
  warn: (msg: string) => void;
}

export interface ForesightPhaseResult {
  proposal: string;
  forecast: string;
  utterances: FlowUtteranceRecord[];
}

/** 長文を行単位で POST_LIMIT 以下に分ける (1 行が長すぎる時だけ文字数で切る)。 */
export function splitForPost(text: string, limit = POST_LIMIT): string[] {
  const chunks: string[] = [];
  let current = "";
  for (const line of text.split("\n")) {
    if (line.length > limit) {
      if (current) chunks.push(current);
      current = "";
      for (let i = 0; i < line.length; i += limit) chunks.push(line.slice(i, i + limit));
      continue;
    }
    const next = current ? `${current}\n${line}` : line;
    if (next.length > limit) {
      chunks.push(current);
      current = line;
    } else {
      current = next;
    }
  }
  if (current) chunks.push(current);
  return chunks;
}

/**
 * 未来予測フェーズを実行する。話者が改革派・保守派の両方に揃わなければ null (行わない)。
 */
export async function runForesightPhase(args: ForesightPhaseArgs): Promise<ForesightPhaseResult | null> {
  const { theme, sessionId, paperId, flow, llm, conclusion, model, commit, log, warn } = args;
  const cast = buildForesightCast(args.utterances);
  if (!cast) {
    log("未来予測: 改革派と保守派に分けられる話者がいないため行わない");
    return null;
  }

  const round = args.utterances.reduce((max, u) => Math.max(max, u.round), 0) + 1;
  const records: FlowUtteranceRecord[] = [];
  let turn = 0;

  const post = async (
    speaker: { personaId: string; personaName: string; role: string },
    stance: string,
    text: string,
    isError: boolean
  ): Promise<void> => {
    for (const chunk of splitForPost(text)) {
      const record: FlowUtteranceRecord = {
        id: randomUUID(),
        sessionId,
        paperId,
        round,
        turn: turn++,
        personaId: speaker.personaId,
        personaName: speaker.personaName,
        role: speaker.role,
        stance,
        text: chunk,
        isError,
      };
      records.push(record);
      await commit(record);
    }
  };

  const ask = async (
    location: string,
    speaker: { personaName: string; role: string },
    p: { system: string; prompt: string }
  ): Promise<{ ok: true; text: string } | { ok: false; error: string }> => {
    const logged = withCostLog(llm, {
      flow,
      sessionId,
      round,
      turn,
      role: speaker.role,
      persona: speaker.personaName,
      location,
    });
    const res = await logged.invoke({ system: p.system, prompt: p.prompt, model });
    return res.ok ? { ok: true, text: res.text.trim() } : { ok: false, error: res.error };
  };

  log(`未来予測 開始 (改革派 ${countCamp(cast, "reform")} 人 / 保守派 ${countCamp(cast, "conservative")} 人)`);

  // ── [F1] 破壊可能な一手の提案 ──
  const proposalRes = await ask("foresight-proposal", cast.facilitator, buildProposalPrompt(theme, conclusion));
  if (!proposalRes.ok || !proposalRes.text) {
    const reason = proposalRes.ok ? "空応答" : proposalRes.error;
    warn(`未来予測: 提案の生成に失敗 (${reason})`);
    await post(cast.facilitator, "neutral", `[エラー: 未来予測の提案を生成できませんでした (${reason})]`, true);
    return { proposal: "", forecast: "", utterances: records };
  }
  const proposal = proposalRes.text;
  await post(cast.facilitator, "neutral", `🔮 未来予測: 破壊的な一手の提案\n${proposal}`, false);

  // ── [F2] 改革派 / 保守派の議論 ──
  const lines: ForesightLine[] = [];
  for (const speaker of buildCampTurnOrder(cast.speakers, args.turns)) {
    const res = await ask(
      "foresight-camp",
      speaker,
      buildCampTurnPrompt({ theme, proposal, personaName: speaker.personaName, camp: speaker.camp, lines })
    );
    if (!res.ok) {
      warn(`未来予測: ${speaker.personaName} の発言に失敗 (${res.error})`);
      await post(speaker, speaker.camp, `[エラー: ${res.error}]`, true);
      continue;
    }
    if (!res.text) continue;
    lines.push({ personaName: speaker.personaName, camp: speaker.camp, text: res.text });
    await post(speaker, speaker.camp, res.text, false);
  }

  // ── [F3] 3 / 5 / 10 年後の予測 + ビジネス分析 ──
  const forecastRes = await ask(
    "foresight-forecast",
    cast.facilitator,
    buildForecastPrompt({ theme, conclusion, proposal, lines })
  );
  let forecast = "";
  if (forecastRes.ok && forecastRes.text) {
    forecast = forecastRes.text;
    await post(cast.facilitator, "neutral", `🔭 未来予測: 3 / 5 / 10 年後\n${forecast}`, false);
  } else {
    const reason = forecastRes.ok ? "空応答" : forecastRes.error;
    warn(`未来予測: 予測の生成に失敗 (${reason})`);
    await post(cast.facilitator, "neutral", `[エラー: 未来予測を生成できませんでした (${reason})]`, true);
  }

  if (args.paperBody) {
    try {
      args.paperBody.set(appendForesightMarkdown(args.paperBody.get() ?? "", { proposal, forecast }));
    } catch (e) {
      warn(`未来予測: ペーパーへの追記に失敗 (${(e as Error).message})`);
    }
  }

  log(`未来予測 終了 (${records.length} 発話)`);
  return { proposal, forecast, utterances: records };
}

function countCamp(cast: ForesightCast, camp: "reform" | "conservative"): number {
  return cast.speakers.filter((s) => s.camp === camp).length;
}
