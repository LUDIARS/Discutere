/**
 * 未来予測アドバイザリー (spec/feature/flow/foresight.md) の進行。
 * 結論を変えない参考として、本議論 (議論 / 改善、rounds / dialectic どちらのエンジンでも) の結論の後に、
 *   [F1] 進行役が 前提事実 → 理論の当てはめ → 破壊可能な一手 + 起きること + リスク を提案
 *   [F2] 改革派 (賛同) と 保守派 (否定) が 成立条件 → 反証 → 確かめ方 の順に交互に論じる
 *   [F3] 記録係が 未解決の前提 + 3 / 5 / 10 年時点の条件付きシナリオ (+ ビジネス分析) をまとめる
 * を 1 ラウンド分の発話として流す。発話は本議論と同じ flow_utterance に永続し onUtterance で通知する。
 * 提案と予測はペーパー本文末尾の「# 未来予測アドバイザリー (参考)」節にも焼く。
 *
 * LLM 失敗は発話にエラーを出して続ける (OVERVIEW §10)。提案が出なければ以降を行わない。
 * 結果の status で 完了 / 一部失敗 / 失敗 を返し、Web の状態表示に使う。
 */

import { randomUUID } from "node:crypto";
import type { LLMClient } from "../../persona-engine/llm/client.js";
import type { FlowUtteranceRecord } from "../director.js";
import { withCostLog } from "../cost-logger.js";
import { buildForesightCast, buildCampTurnOrder, campStageAt, type ForesightCast } from "./camps.js";
import {
  buildProposalPrompt,
  buildCampTurnPrompt,
  buildForecastPrompt,
  type ForesightContext,
  type ForesightLine,
} from "./prompts.js";
import { appendForesightMarkdown } from "./markdown.js";

/** Discord の 1 投稿 (webhook は 1900 字で切る) に収める上限 (分割番号の分を空けている)。 */
const POST_LIMIT = 1780;

export interface ForesightPhaseArgs {
  theme: string;
  sessionId: string;
  paperId: string;
  flow: string;
  llm: LLMClient;
  /** 本議論の発話 (話者の引き継ぎ元)。 */
  utterances: readonly FlowUtteranceRecord[];
  /** 本議論の結論 (結論なしなら空でよい)。 */
  conclusion: string;
  /** 本議論の文脈 (ペーパー本文 + 外部の声)。提案と予測に渡す。 */
  context?: ForesightContext;
  /** 改革派 / 保守派の発言回数 (往復単位に揃える)。 */
  turns: number;
  /** 提案と議論に使うモデル (undefined なら LLM の既定)。 */
  model?: string;
  /** 予測に使うモデル (undefined なら model)。 */
  forecastModel?: string;
  /** 発話の永続 + 通知 (本議論と同じ経路)。 */
  commit: (record: FlowUtteranceRecord) => Promise<void>;
  /** ペーパー本文の読み書き (「# 未来予測アドバイザリー (参考)」節を焼く)。 */
  paperBody?: { get(): string | null; set(bodyMd: string): void };
  log: (msg: string) => void;
  warn: (msg: string) => void;
}

/** done = 全部出た / degraded = 一部の発言か予測が失敗 / failed = 提案が出ず止めた。 */
export type ForesightPhaseStatus = "done" | "degraded" | "failed";

export interface ForesightPhaseResult {
  proposal: string;
  forecast: string;
  utterances: FlowUtteranceRecord[];
  status: ForesightPhaseStatus;
  /** 失敗した段の数 (議論の発言 + 予測 + ペーパー追記)。 */
  failures: number;
}

/**
 * 長文を行単位で limit 以下に分ける (1 行が長すぎる時だけ文字数で切る)。
 * 文字数はコードポイントで数え、サロゲートペアを割らない。
 */
export function splitForPost(text: string, limit = POST_LIMIT): string[] {
  const chunks: string[] = [];
  let current = "";
  const len = (s: string) => Array.from(s).length;
  for (const line of text.split("\n")) {
    if (len(line) > limit) {
      if (current) chunks.push(current);
      current = "";
      const chars = Array.from(line);
      for (let i = 0; i < chars.length; i += limit) chunks.push(chars.slice(i, i + limit).join(""));
      continue;
    }
    const next = current ? `${current}\n${line}` : line;
    if (len(next) > limit) {
      chunks.push(current);
      current = line;
    } else {
      current = next;
    }
  }
  if (current) chunks.push(current);
  return chunks;
}

/** 複数に分けた時だけ「(n/m)」を末尾に付ける (どの投稿が 1 発言の続きか分かるように)。 */
export function labelParts(chunks: readonly string[]): string[] {
  if (chunks.length <= 1) return [...chunks];
  return chunks.map((c, i) => `${c}\n(${i + 1}/${chunks.length})`);
}

/**
 * 未来予測フェーズを実行する。進行役以外の話者が 2 人未満なら null (行わない)。
 */
export async function runForesightPhase(args: ForesightPhaseArgs): Promise<ForesightPhaseResult | null> {
  const { theme, sessionId, paperId, flow, llm, conclusion, context, model, commit, log, warn } = args;
  const cast = buildForesightCast(args.utterances);
  if (!cast) {
    log("未来予測: 改革派と保守派に分けられる話者がいないため行わない");
    return null;
  }

  const round = args.utterances.reduce((max, u) => Math.max(max, u.round), 0) + 1;
  const records: FlowUtteranceRecord[] = [];
  let turn = 0;
  let failures = 0;

  const post = async (
    speaker: { personaId: string; personaName: string; role: string },
    stance: string,
    text: string,
    isError: boolean
  ): Promise<void> => {
    for (const chunk of labelParts(splitForPost(text))) {
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
    p: { system: string; prompt: string },
    useModel: string | undefined
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
    const res = await logged.invoke({ system: p.system, prompt: p.prompt, model: useModel });
    return res.ok ? { ok: true, text: res.text.trim() } : { ok: false, error: res.error };
  };

  log(`未来予測 開始 (改革派 ${countCamp(cast, "reform")} 人 / 保守派 ${countCamp(cast, "conservative")} 人)`);

  // ── [F1] 破壊可能な一手の提案 ──
  const proposalRes = await ask(
    "foresight-proposal",
    cast.facilitator,
    buildProposalPrompt(theme, conclusion, context),
    model
  );
  if (!proposalRes.ok || !proposalRes.text) {
    const reason = proposalRes.ok ? "空応答" : proposalRes.error;
    warn(`未来予測: 提案の生成に失敗 (${reason})`);
    await post(cast.facilitator, "neutral", `[エラー: 未来予測の提案を生成できませんでした (${reason})]`, true);
    return { proposal: "", forecast: "", utterances: records, status: "failed", failures: 1 };
  }
  const proposal = proposalRes.text;
  await post(cast.facilitator, "neutral", `🔮 未来予測アドバイザリー (参考): 破壊的な一手の提案\n${proposal}`, false);

  // ── [F2] 改革派 / 保守派の議論 (往復ごとに 成立条件 → 反証 → 確かめ方) ──
  const lines: ForesightLine[] = [];
  const order = buildCampTurnOrder(cast.speakers, args.turns);
  for (let i = 0; i < order.length; i++) {
    const speaker = order[i];
    const res = await ask(
      "foresight-camp",
      speaker,
      buildCampTurnPrompt({
        theme,
        proposal,
        personaName: speaker.personaName,
        camp: speaker.camp,
        lines,
        stage: campStageAt(i),
        ownStatements: speaker.ownStatements,
      }),
      model
    );
    if (!res.ok) {
      failures++;
      warn(`未来予測: ${speaker.personaName} の発言に失敗 (${res.error})`);
      await post(speaker, speaker.camp, `[エラー: ${res.error}]`, true);
      continue;
    }
    if (!res.text) {
      failures++;
      warn(`未来予測: ${speaker.personaName} の発言が空応答`);
      continue;
    }
    lines.push({ personaName: speaker.personaName, camp: speaker.camp, text: res.text });
    await post(speaker, speaker.camp, res.text, false);
  }

  // ── [F3] 未解決の前提 + 3 / 5 / 10 年時点のシナリオ + ビジネス分析 ──
  const forecastRes = await ask(
    "foresight-forecast",
    cast.facilitator,
    buildForecastPrompt({ theme, conclusion, proposal, lines, context }),
    args.forecastModel ?? model
  );
  let forecast = "";
  if (forecastRes.ok && forecastRes.text) {
    forecast = forecastRes.text;
    await post(cast.facilitator, "neutral", `🔭 未来予測アドバイザリー (参考): 3 / 5 / 10 年後\n${forecast}`, false);
  } else {
    failures++;
    const reason = forecastRes.ok ? "空応答" : forecastRes.error;
    warn(`未来予測: 予測の生成に失敗 (${reason})`);
    await post(cast.facilitator, "neutral", `[エラー: 未来予測を生成できませんでした (${reason})]`, true);
  }

  if (args.paperBody) {
    try {
      args.paperBody.set(appendForesightMarkdown(args.paperBody.get() ?? "", { proposal, forecast }));
    } catch (e) {
      failures++;
      warn(`未来予測: ペーパーへの追記に失敗 (${(e as Error).message})`);
    }
  }

  log(`未来予測 終了 (${records.length} 発話、失敗 ${failures})`);
  return { proposal, forecast, utterances: records, status: failures > 0 ? "degraded" : "done", failures };
}

function countCamp(cast: ForesightCast, camp: "reform" | "conservative"): number {
  return cast.speakers.filter((s) => s.camp === camp).length;
}
