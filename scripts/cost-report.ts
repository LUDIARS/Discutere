/**
 * LLM コスト/トークン集計 CLI。
 *
 *   npm run cost-report                       # 全期間の合計 + backend/flow/session 別
 *   npm run cost-report -- --session <id>     # 単一セッションに絞る
 *   npm run cost-report -- --since <epochMs>  # created_at >= since
 *   npm run cost-report -- --json             # JSON で出力 (機械処理用)
 *
 * llm_call_log を集計する (内部検証用)。cost_usd は claude-cli(サブスク) では
 * 等価 API 換算の推定値、anthropic 従量経路では通常 NULL=0。
 */

import { getFlowDb } from "../src/flow/db/connection.js";
import {
  summarizeCost,
  formatCostReport,
  type CostReportFilter,
} from "../src/flow/cost-report.js";

function parseArgs(argv: string[]): { filter: CostReportFilter; json: boolean } {
  const filter: CostReportFilter = {};
  let json = false;
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === "--session") filter.sessionId = argv[++i];
    else if (a === "--since") filter.since = Number(argv[++i]);
    else if (a === "--json") json = true;
  }
  return { filter, json };
}

const { filter, json } = parseArgs(process.argv.slice(2));
const db = getFlowDb();
const report = summarizeCost(db, filter);

if (json) {
  console.log(JSON.stringify(report, null, 2));
} else {
  console.log(formatCostReport(report));
}
