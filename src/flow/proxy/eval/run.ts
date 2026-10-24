/**
 * 代理 1 人ぶんの本人らしさの評価を回す (S4)。LLM は注入 (テストはモック)。
 *
 *   - M3 判別 (必須): 伏せた発言 (評価専用ファイル) を、本人と相手の束のどちらか当てさせる。
 *     本人を 1 番 / 2 番のどちらに置くかは発言ごとに交互 (位置の偏りを打ち消す)
 *   - M1 推薦予測 (参考、recommend=true のとき): パックの heldOut を束だけで当てさせる
 *   - M5/M6: 代理議論の発話 (flow_utterance の claim_kind / evidence_ids_json) から
 * 判定は judgeProxyEval。結果の書き戻しは呼び出し側 (scripts/eval-proxy.ts --write)。
 */

import type { LLMClient } from "../../../persona-engine/llm/client.js";
import { getFlowDb } from "../../db/connection.js";
import { renderProxyBundle } from "../bundle.js";
import { isGroundedKind, type ClaimKind } from "../attribution.js";
import type { ProxyProfileRow } from "../store.js";
import { askDiscriminate, askRecommend, askSupported } from "./probes.js";
import {
  judgeProxyEval,
  rate,
  scoreRecommendations,
  type ProxyEvalMetrics,
  type ProxyEvalThresholds,
  type ProxyEvalVerdict,
  type RecommendAnswer,
} from "./metrics.js";

export interface ProxyEvalReport {
  label: string;
  genre: string;
  metrics: ProxyEvalMetrics;
  verdict: ProxyEvalVerdict;
}

export interface EvaluateProxyArgs {
  row: ProxyProfileRow;
  /** 判別テストの相手 (同じジャンルの束にできる別の人物)。無ければ M3 は測らない。 */
  partner?: ProxyProfileRow;
  /** この人物の伏せた発言 (評価専用)。無ければ M3 は測らない。 */
  heldOutTexts?: readonly string[];
  /** M3 に使う伏せた発言の上限 (LLM 呼び出し数を抑える)。既定 12。 */
  maxTexts?: number;
  /** 推薦予測 (M1、参考値) も測るか。既定 false。 */
  recommend?: boolean;
  /** 代理議論のセッション id (M5/M6)。無ければ測らない。 */
  sessionIds?: readonly string[];
  budgetTokens: number;
  llm: LLMClient;
  model?: string;
  thresholds?: ProxyEvalThresholds;
}

/** 代理議論に出すのと同じ束。束にできない人物 (confidence=low 等) は代理に出ないので評価しない。 */
function bundleOf(row: ProxyProfileRow, budgetTokens: number): string | null {
  return renderProxyBundle(row.pack, { budgetTokens, label: row.label })?.text ?? null;
}

/** 等間隔に最大 max 件を選ぶ (先頭のゲームに偏らせない)。 */
export function sampleEvenly<T>(items: readonly T[], max: number): T[] {
  if (items.length <= max) return [...items];
  return Array.from({ length: max }, (_, i) => items[Math.floor((i * items.length) / max)]);
}

interface UtteranceRow {
  text: string;
  claim_kind: string | null;
  evidence_ids_json: string | null;
}

/** M5/M6 の材料: 指定セッションのこの代理の発話。 */
function proxyUtterances(personaId: string, sessionIds: readonly string[]): UtteranceRow[] {
  if (sessionIds.length === 0) return [];
  const placeholders = sessionIds.map(() => "?").join(",");
  return getFlowDb()
    .prepare(
      `SELECT text, claim_kind, evidence_ids_json FROM flow_utterance
        WHERE persona_id = ? AND session_id IN (${placeholders}) AND claim_kind IS NOT NULL`
    )
    .all(personaId, ...sessionIds) as UtteranceRow[];
}

export async function evaluateProxy(args: EvaluateProxyArgs): Promise<ProxyEvalReport> {
  const { row, llm, model } = args;
  const bundle = bundleOf(row, args.budgetTokens);
  if (bundle === null) {
    return {
      label: row.label,
      genre: row.genre,
      metrics: { discrimination: { total: 0, correct: 0, accuracy: null }, recommendation: null, groundedRate: null, misattributionRate: null },
      verdict: { status: "unevaluated", reasons: ["材料が少なく束にできない (代理に出ない)"] },
    };
  }

  // M3
  const texts = sampleEvenly(args.heldOutTexts ?? [], args.maxTexts ?? 12);
  const other = args.partner ? bundleOf(args.partner, args.budgetTokens) : null;
  let correct = 0;
  let total = 0;
  if (other !== null) {
    for (let i = 0; i < texts.length; i++) {
      const selfFirst = i % 2 === 0;
      const answer = await askDiscriminate(llm, selfFirst ? [bundle, other] : [other, bundle], texts[i], model);
      if (answer === (selfFirst ? 1 : 2)) correct++;
      total++;
    }
  }
  const discrimination = { total, correct, accuracy: rate(correct, total) };

  // M1 (参考)
  let recommendation: ReturnType<typeof scoreRecommendations> | null = null;
  if (args.recommend) {
    const heldOut = sampleEvenly(row.pack.heldOut, args.maxTexts ?? 12);
    const answers: RecommendAnswer[] = [];
    for (const h of heldOut) answers.push(await askRecommend(llm, bundle, h.game, model));
    recommendation = scoreRecommendations(heldOut.map((h) => h.recommended), answers, row.pack.sufficiency);
  }

  // M5 / M6
  const personaId = `proxy:${row.speakerKey}:${row.genre}`;
  const utterances = proxyUtterances(personaId, args.sessionIds ?? []);
  const grounded = utterances.filter((u) => isGroundedKind(u.claim_kind as ClaimKind));
  let unsupported = 0;
  for (const u of grounded) {
    const ids = u.evidence_ids_json ? (JSON.parse(u.evidence_ids_json) as string[]) : [];
    const evidence = ids.map((id) => row.pack.evidence.find((e) => e.id === id)?.text ?? "").filter(Boolean);
    if (!(await askSupported(llm, evidence, u.text, model))) unsupported++;
  }
  const metrics: ProxyEvalMetrics = {
    discrimination,
    recommendation,
    groundedRate: rate(grounded.length, utterances.length),
    misattributionRate: rate(unsupported, grounded.length),
  };
  return { label: row.label, genre: row.genre, metrics, verdict: judgeProxyEval(metrics, args.thresholds) };
}
