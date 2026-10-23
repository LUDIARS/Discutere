/**
 * 代理 1 人ぶんの本人らしさの評価を回す (S4)。LLM は注入 (テストはモック)。
 *
 *   - M1 推薦予測: パックの heldOut (伏せたゲーム + 本人の推薦可否) を束だけで当てさせる
 *   - M3 判別: 伏せた発言 (評価専用ファイル) を、本人と相手の束のどちらか当てさせる。
 *     本人を 1 番 / 2 番のどちらに置くかは発言ごとに交互 (位置の偏りを打ち消す)
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
  /** 判別テストの相手 (同じジャンルの別の人物)。無ければ M3 は測らない。 */
  partner?: ProxyProfileRow;
  /** この人物の伏せた発言 (評価専用)。無ければ M3 は測らない。 */
  heldOutTexts?: readonly string[];
  /** 代理議論のセッション id (M5/M6)。無ければ測らない。 */
  sessionIds?: readonly string[];
  budgetTokens: number;
  llm: LLMClient;
  model?: string;
  thresholds?: ProxyEvalThresholds;
}

function bundleOf(row: ProxyProfileRow, budgetTokens: number): string {
  // 評価は confidence=low でも測れるよう、束にできなければ根拠を含まない最小形で問う (合否は M1 の件数で決まる)。
  return renderProxyBundle(row.pack, { budgetTokens, label: row.label })?.text ?? `# 人物: ${row.label}\n(材料なし)`;
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

  // M1
  const labels = row.pack.heldOut.map((h) => h.recommended);
  const answers: RecommendAnswer[] = [];
  for (const h of row.pack.heldOut) answers.push(await askRecommend(llm, bundle, h.game, model));
  const recommendation = scoreRecommendations(labels, answers, row.pack.sufficiency);

  // M3
  let discrimination: number | null = null;
  const texts = args.heldOutTexts ?? [];
  if (args.partner && texts.length > 0) {
    const other = bundleOf(args.partner, args.budgetTokens);
    let correct = 0;
    for (let i = 0; i < texts.length; i++) {
      const selfFirst = i % 2 === 0;
      const answer = await askDiscriminate(llm, selfFirst ? [bundle, other] : [other, bundle], texts[i], model);
      if (answer === (selfFirst ? 1 : 2)) correct++;
    }
    discrimination = rate(correct, texts.length);
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
    recommendation,
    discrimination,
    groundedRate: rate(grounded.length, utterances.length),
    misattributionRate: rate(unsupported, grounded.length),
  };
  return { label: row.label, genre: row.genre, metrics, verdict: judgeProxyEval(metrics, args.thresholds) };
}
